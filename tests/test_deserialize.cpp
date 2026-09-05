// Host test for the RX deserialization path:
//  - deserialize_transfer must pass transfer->payload_size (not TypeInfo::extent)
//    to the deserializer;
//  - both subscription flavors must NOT invoke the user handler when
//    deserialization fails (the error handler is allowed to just return).
//
// Build & run via `make test`.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "cyphal/cyphal.h"
#include "cyphal/allocators/sys/sys_allocator.h"
#include "cyphal/subscriptions/callbacks.h"
#include "cyphal/subscriptions/subscription.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::cerr << "FAIL " << __LINE__ << ": " << #cond << std::endl;                      \
            g_failures++;                                                                        \
        }                                                                                        \
    } while (0)

// --- Test payload: 4 bytes on the wire, extent deliberately larger (8) so a
// deserialize that used extent instead of payload_size is observable.
struct TestPayload {
    uint32_t value;
};

size_t g_last_deserialize_size = 0;

int8_t test_serialize(const TestPayload* obj, uint8_t* buffer, size_t* size) {
    std::memcpy(buffer, &obj->value, 4);
    *size = 4;
    return 0;
}

int8_t test_deserialize(TestPayload* obj, const uint8_t* buffer, size_t* size) {
    g_last_deserialize_size = *size;
    if (*size < 4) {
        return -1;
    }
    std::memcpy(&obj->value, buffer, 4);
    *size = 4;
    return 0;
}

}  // namespace

template <>
struct CyphalTypeTraits<TestPayload> {
    static constexpr cyphal_serializer<TestPayload> serializer = test_serialize;
    static constexpr cyphal_deserializer<TestPayload> deserializer = test_deserialize;
    static constexpr size_t extent = 8;
    static constexpr size_t buffer_size = 4;
};

namespace {

int g_error_count = 0;

uint64_t test_micros() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

class MockCAN : public AbstractCANProvider {
public:
    struct Captured {
        CanardFrame frame;
        std::array<uint8_t, 64> data;
    };
    std::vector<Captured> tx_frames;

    MockCAN(size_t queue_len, const UtilityConfig& utilities, CanardNodeID node_id)
        : AbstractCANProvider(CANARD_MTU_CAN_FD, 64, queue_len, utilities) {
        setup<SystemAllocator>(
            new SystemAllocator(queue_len * sizeof(CanardTxQueueItem) * QUEUE_SIZE_MULT, utilities),
            node_id,
            delete_allocator
        );
    }

    uint32_t len_to_dlc(size_t) override {
        return 0;
    }
    size_t dlc_to_len(uint32_t) override {
        return 0;
    }
    void can_loop(bool) override {}
    bool read_frame(CanardFrame*, void*) override {
        return false;
    }
    int write_frame(const CanardTxQueueItem* ti) override {
        Captured captured;
        std::memcpy(captured.data.data(), ti->frame.payload, ti->frame.payload_size);
        captured.frame = ti->frame;
        captured.frame.payload = nullptr;  // re-pointed to data in feed()
        tx_frames.push_back(captured);
        return static_cast<int>(ti->frame.payload_size);
    }

    void feed(const Captured& captured) {
        CanardFrame frame = captured.frame;
        frame.payload = const_cast<uint8_t*>(captured.data.data());
        process_canard_rx(&frame);
    }
};

class TestSub : public AbstractSubscription<TestPayload> {
public:
    int calls = 0;
    TestSub(InterfacePtr& interface, CanardPortID port_id)
        : AbstractSubscription(interface, port_id) {}

private:
    void handler(const TestPayload&, CanardRxTransfer*) override {
        calls++;
    }
};

}  // namespace

int main() {
    UtilityConfig utilities(test_micros, []() { g_error_count++; });
    auto* mock = new MockCAN(32, utilities, 42);
    auto iface = std::shared_ptr<CyphalInterface>(
        new CyphalInterface(42, utilities, mock, delete_provider)
    );

    // --- deserialize_transfer: payload_size is passed, bool result reflects errors
    {
        TestPayload obj{};
        uint8_t buf[4] = {1, 2, 3, 4};
        CanardRxTransfer transfer = {};
        transfer.payload = buf;
        transfer.payload_size = 4;
        CHECK(iface->deserialize_transfer<TestPayload>(&obj, &transfer));
        CHECK(g_last_deserialize_size == 4);  // payload_size, not extent (8)
        CHECK(obj.value == 0x04030201);

        transfer.payload_size = 1;
        int errors_before = g_error_count;
        CHECK(!iface->deserialize_transfer<TestPayload>(&obj, &transfer));
        CHECK(g_last_deserialize_size == 1);
        CHECK(g_error_count == errors_before + 1);
    }

    // --- callback subscription: valid frame invokes callback; truncated frame does not
    int cb_calls = 0;
    uint32_t cb_value = 0;
    iface->subscribe<TestPayload>(
        5000,
        [&](const TestPayload& payload, CanardRxTransfer*) {
            cb_calls++;
            cb_value = payload.value;
        }
    );

    TestPayload out{0xDEADBEEF};
    CanardTransferID tid = 0;
    iface->send_msg(&out, 5000, &tid);
    iface->process_tx_once();
    CHECK(mock->tx_frames.size() == 1);

    int errors_before = g_error_count;
    mock->feed(mock->tx_frames[0]);
    CHECK(cb_calls == 1);
    CHECK(cb_value == 0xDEADBEEF);
    CHECK(g_last_deserialize_size == 4);  // wire size, not extent (8)
    CHECK(g_error_count == errors_before);

    // Craft a truncated transfer: 1 byte of payload + tail byte (start+end, next tid)
    {
        MockCAN::Captured bad = mock->tx_frames[0];
        bad.data[0] = 0xAB;
        bad.data[1] = 0xE0 | 1;
        bad.frame.payload_size = 2;
        errors_before = g_error_count;
        mock->feed(bad);
        CHECK(cb_calls == 1);  // callback skipped
        CHECK(g_last_deserialize_size == 1);
        CHECK(g_error_count == errors_before + 1);
    }

    // --- AbstractSubscription: same guarantees for the virtual-handler flavor
    {
        TestSub sub(iface, 5001);

        uint8_t one_byte = 0xAB;
        CanardRxTransfer transfer = {};
        transfer.payload = &one_byte;
        transfer.payload_size = 1;
        errors_before = g_error_count;
        sub.accept(&transfer);
        CHECK(sub.calls == 0);  // handler skipped
        CHECK(g_error_count == errors_before + 1);

        uint8_t four_bytes[4];
        uint32_t value = 0x12345678;
        std::memcpy(four_bytes, &value, 4);
        transfer.payload = four_bytes;
        transfer.payload_size = 4;
        errors_before = g_error_count;
        sub.accept(&transfer);
        CHECK(sub.calls == 1);
        CHECK(g_last_deserialize_size == 4);
        CHECK(g_error_count == errors_before);
    }

    if (g_failures == 0) {
        std::cout << "All tests passed" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
}
