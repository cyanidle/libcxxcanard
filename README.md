# libcxxcanard

`libcxxcanard` is a small C++17 wrapper around
[libcanard](https://github.com/OpenCyphal/libcanard). It keeps the transport
model close to libcanard, while hiding repetitive serialization, subscription,
and provider setup code.

The current high-level API is:

- `CyphalInterface` owns the CAN provider, allocator, queues, and subscriptions.
- `EmbeddedCyphal` derives from `CyphalInterface`; `ArduinoCyphal` derives from `EmbeddedCyphal`.
- `make_cyphal<T>(...)` constructs an embedded node with shared ownership and `shared_from_this()` support.
- `AbstractSubscription<T>` supports class-style handlers.
- `subscribe(port, callback)` and `subscribe(port, kind, callback)` are available for callback-style handlers.
- `cyphal_common_types.hpp` provides short aliases such as `UInt32`, `Float32`, `CyphalString`, `Bytes`, and `Empty`.
  Non-Arduino builds also expose `String` as an alias for `CyphalString`; Arduino keeps its own `String`.

## Platform Notes

### Arduino

Arduino uses the packed library in `src/`. Regenerate it after changing library
headers or generated type includes:

```sh
make arduino-lib
```

Open the sketches from [examples/Arduino](examples/Arduino/README.md). The
minimal path is:

```cpp
#include <VBCoreG4_arduino_system.h>
#include <cyphal.h>
#include <cyphal_common_types.hpp>

CanFD canfd;
std::shared_ptr<ArduinoCyphal<>> cyphal;

void setup() {
    SystemClock_Config();
    canfd.init();
    canfd.write_default_params();
    canfd.apply_config();

    cyphal = make_cyphal<ArduinoCyphal<>>(
        canfd.get_hfdcan(),
        42,
        "org.example.node"
    );
    cyphal->begin();
}

void loop() {
    cyphal->cyphal_loop();
}
```

### Linux / CMake

Linux uses SocketCAN through `cyphal/providers/LinuxCAN.h`. The examples in
[examples/Linux](examples/Linux/README.md) are regular CMake executables and
show how to include `libcxxcanard` and project DSDL repositories.

Required tools:

- CMake with C++17 compiler.
- `nnvg` or `uv` for Nunavut code generation.
- SocketCAN interface, for example `vcan1.3` or `can0`.

Typical CMake shape:

```cmake
find_package(Threads REQUIRED)

add_subdirectory(libcxxcanard)
include(libcxxcanard/examples/cyphal_types.cmake)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE libcxxcanard Threads::Threads)
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libcxxcanard
    ${CMAKE_CURRENT_SOURCE_DIR}/libcxxcanard/libs
)
cyphal_add_dsdl_types(app
    TYPE_REPOS
        ${CMAKE_CURRENT_SOURCE_DIR}/cyphal_types/public_regulated_data_types
        ${CMAKE_CURRENT_SOURCE_DIR}/cyphal_types/voltbro_types
)
```

### STM32 HAL

Bare-metal STM32 projects use `cyphal/providers/G4CAN.h` directly. The
platform-specific example folder is reserved at
[examples/STM32HAL](examples/STM32HAL/README.md).

## Callback-Style Subscribe

`subscribe(port, callback)` subscribes to messages. The payload type is inferred
from the callback argument.

```cpp
cyphal->subscribe(5000, [](const UInt32& msg, CanardRxTransfer*) {
    Serial.println(msg.value);
});
```

For services or non-message transfers, pass the kind explicitly:

```cpp
using EchoRequest = voltbro_echo_echo_service_Request_1_0;
using EchoResponse = voltbro_echo_echo_service_Response_1_0;

cyphal->subscribe(101, CanardTransferKindRequest,
    [&](const EchoRequest& request, CanardRxTransfer* transfer) {
        EchoResponse response{};
        response.pong = request.ping;
        cyphal->send_response(&response, transfer);
    }
);
```

If type deduction is not possible, pass the payload type:

```cpp
cyphal->subscribe<UInt32>(5000, my_callable);
```

## Class-Style Subscribe

Class-style subscriptions are still useful when the handler owns state or is
part of a larger node class.

```cpp
class UInt32Sub : public AbstractSubscription<UInt32> {
public:
    explicit UInt32Sub(InterfacePtr interface)
        : AbstractSubscription<UInt32>(interface, 5000) {}

private:
    void handler(const UInt32& msg, CanardRxTransfer*) override {
        Serial.println(msg.value);
    }
};

class AppCyphal : public ArduinoCyphal<> {
public:
    using ArduinoCyphal<>::ArduinoCyphal;

private:
    UInt32Sub* sub = nullptr;

    void setup_subscriptions() override {
        ArduinoCyphal<>::setup_subscriptions();
        sub = new UInt32Sub(shared_from_this());
    }
};
```

## Sending Transfers

Messages use broadcast destination and a transfer-ID per subject:

```cpp
static CanardTransferID transfer_id = 0;

UInt32 msg{};
msg.value = 123;
cyphal->send_msg(&msg, 5000, &transfer_id);
```

Requests need a remote node-ID:

```cpp
using EchoRequest = voltbro_echo_echo_service_Request_1_0;

static CanardTransferID transfer_id = 0;

EchoRequest request{};
fill_string(request.ping, "ping");
cyphal->send_request(&request, 101, &transfer_id, target_node_id);
```

Responses reuse metadata from the incoming request:

```cpp
using EchoRequest = voltbro_echo_echo_service_Request_1_0;
using EchoResponse = voltbro_echo_echo_service_Response_1_0;

void on_echo_request(const EchoRequest& request, CanardRxTransfer* transfer) {
    EchoResponse response{};
    response.pong = request.ping;
    cyphal->send_response(&response, transfer);
}
```

## Examples

- [Arduino examples](examples/Arduino/README.md): minimal message echo, minimal service echo, and full embedded node.
- [Linux examples](examples/Linux/README.md): SocketCAN peers that talk to the Arduino sketches.
- [STM32 HAL examples](examples/STM32HAL/README.md): placeholder for direct HAL projects.


## Bounded transport servicing

The provider exposes these servicing operations. `CyphalInterface` forwards
`process_canard_rx`, `process_canard_tx`, and `update_fdcan_status` to its provider.

| Operation | Contract |
| --- | --- |
| `process_canard_rx(bool all = true)` | Poll RX and dispatch completed transfers to subscription handlers. `false` consumes at most one CAN frame; a multi-frame transfer needs multiple calls. |
| `process_canard_tx(bool all = true)` | Process the TX queue. `false` handles at most one queue item, including an expired item. Processing returns when the provider cannot accept a frame; unexpired queued frames remain pending. |
| `update_fdcan_status()` | Update provider status and invoke its configured error handling. Providers without a hardware status operation implement a no-op. |
| `can_loop(bool no_tx = false)` | Service RX and, unless `no_tx` is set, TX. G4CAN also updates FDCAN status. |
| Protected `accept_rx_frame(const CanardFrame&)` | Submit an acquired CAN frame to libcanard, perform transfer reassembly and dispatch completed transfers. |

For a bounded main-loop pass, call RX with `false`, perform application reporting,
call TX with `false`, then update provider status. One RX frame may complete a
transfer and run its handler; frame count does not bound the handler's duration.

### Shared allocator arena

An external operation may reuse an O1Allocator arena through this lifecycle:

1. Stop transport processing and hardware access to the arena.
2. Call `halt()` to release all TX/RX allocations and receive sessions.
   Subscription descriptors, listeners and node configuration remain registered.
3. Give the external operation exclusive use of the arena.
4. Call `reset()` to restore the allocator in the returned arena,
   discard pending hardware frames, and resume transport.

Subscription descriptors and listeners must be stored outside the reused arena.
No callback or transport thread may access the allocator between `halt()` and
`reset()`. `halt()` releases allocations while the allocator is valid; `reset()`
reinitializes the returned arena without inspecting or freeing its overwritten contents.
O1Allocator resets in the supplied memory without allocating an arena or
subscription objects.
