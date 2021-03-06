// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include "common/logging/log.h"
#include "common/vector_math.h"
#include "input_common/udp/client.h"
#include "input_common/udp/protocol.h"

using boost::asio::ip::address_v4;
using boost::asio::ip::udp;

namespace InputCommon::CemuhookUDP {

struct SocketCallback {
    std::function<void(Response::Version)> version;
    std::function<void(Response::PortInfo)> port_info;
    std::function<void(Response::PadData)> pad_data;
};

class Socket {
public:
    using clock = std::chrono::system_clock;

    explicit Socket(const std::string& host, u16 port, u32 client_id, SocketCallback callback)
        : client_id(client_id), timer(io_service),
          send_endpoint(udp::endpoint(address_v4::from_string(host), port)),
          socket(io_service, udp::endpoint(udp::v4(), 0)), callback(std::move(callback)) {}

    void Stop() {
        io_service.stop();
    }

    void Loop() {
        io_service.run();
    }

    void StartSend(const clock::time_point& from) {
        timer.expires_at(from + std::chrono::seconds(3));
        timer.async_wait([this](const boost::system::error_code& error) { HandleSend(error); });
    }

    void StartReceive() {
        socket.async_receive_from(
            boost::asio::buffer(receive_buffer), receive_endpoint,
            [this](const boost::system::error_code& error, std::size_t bytes_transferred) {
                HandleReceive(error, bytes_transferred);
            });
    }

private:
    void HandleReceive(const boost::system::error_code& error, std::size_t bytes_transferred) {
        if (auto type = Response::Validate(receive_buffer.data(), bytes_transferred)) {
            switch (*type) {
            case Type::Version: {
                Response::Version version;
                std::memcpy(&version, &receive_buffer[sizeof(Header)], sizeof(Response::Version));
                callback.version(std::move(version));
                break;
            }
            case Type::PortInfo: {
                Response::PortInfo port_info;
                std::memcpy(&port_info, &receive_buffer[sizeof(Header)],
                            sizeof(Response::PortInfo));
                callback.port_info(std::move(port_info));
                break;
            }
            case Type::PadData: {
                Response::PadData pad_data;
                std::memcpy(&pad_data, &receive_buffer[sizeof(Header)], sizeof(Response::PadData));
                callback.pad_data(std::move(pad_data));
                break;
            }
            }
        }
        StartReceive();
    }

    void HandleSend(const boost::system::error_code& error) {
        // TODO: add something to the UI to let people choose what ports to listen on
        // Send a request for getting port info for pad 1
        Request::PortInfo port_info{1, {0, 0, 0, 0}};
        auto port_message = Request::Create(port_info, client_id);
        std::memcpy(&send_buffer1, &port_message, PORT_INFO_SIZE);
        size_t len = socket.send_to(boost::asio::buffer(send_buffer1), send_endpoint);

        // Send a request for getting pad data for pad 1
        Request::PadData pad_data{Request::PadData::Flags::Id, 0, EMPTY_MAC_ADDRESS};
        auto pad_message = Request::Create(pad_data, client_id);
        std::memcpy(send_buffer2.data(), &pad_message, PAD_DATA_SIZE);
        size_t len2 = socket.send_to(boost::asio::buffer(send_buffer2), send_endpoint);
        StartSend(timer.expiry());
    }

    SocketCallback callback;
    boost::asio::io_service io_service;
    boost::asio::basic_waitable_timer<clock> timer;
    udp::socket socket;

    u32 client_id;

    static constexpr size_t PORT_INFO_SIZE = sizeof(Message<Request::PortInfo>);
    static constexpr size_t PAD_DATA_SIZE = sizeof(Message<Request::PadData>);
    std::array<u8, PORT_INFO_SIZE> send_buffer1;
    std::array<u8, PAD_DATA_SIZE> send_buffer2;
    udp::endpoint send_endpoint;

    std::array<u8, MAX_PACKET_SIZE> receive_buffer;
    udp::endpoint receive_endpoint;
};

static void SocketLoop(Socket* socket) {
    socket->StartReceive();
    socket->StartSend(Socket::clock::now());
    socket->Loop();
}

Client::Client(std::shared_ptr<DeviceStatus> status, const std::string& host, u16 port,
               u32 client_id)
    : status(status) {
    SocketCallback callback{[this](Response::Version version) { OnVersion(version); },
                            [this](Response::PortInfo info) { OnPortInfo(info); },
                            [this](Response::PadData data) { OnPadData(data); }};
    LOG_INFO(Input, "Starting communication with UDP input server on {}:{}", host, port);
    socket = std::make_unique<Socket>(host, port, client_id, callback);
    thread = std::thread{SocketLoop, this->socket.get()};
}

Client::~Client() {
    socket->Stop();
    thread.join();
}

void Client::OnVersion(Response::Version data) {
    LOG_TRACE(Input, "Version packet received: {}", data.version);
}

void Client::OnPortInfo(Response::PortInfo data) {
    LOG_TRACE(Input, "PortInfo packet received: {}", data.model);
}

void Client::OnPadData(Response::PadData data) {
    LOG_TRACE(Input, "PadData packet received");
    if (data.packet_counter <= packet_sequence) {
        LOG_WARNING(
            Input,
            "PadData packet dropped because its stale info. Current count: {} Packet count: {}",
            packet_sequence, data.packet_counter);
        return;
    }
    packet_sequence = data.packet_counter;
    // Due to differences between the 3ds and cemuhookudp motion directions, we need to invert
    // accel.x and accel.z and also invert pitch and yaw. See
    // https://github.com/citra-emu/citra/pull/4049 for more details on gyro/accel
    Math::Vec3f accel = Math::MakeVec<float>(-data.accel.x, data.accel.y, -data.accel.z);
    Math::Vec3f gyro = Math::MakeVec<float>(-data.gyro.pitch, -data.gyro.yaw, data.gyro.roll);
    {
        std::lock_guard<std::mutex> guard(status->update_mutex);

        status->motion_status = {accel, gyro};

        // TODO: add a setting for "click" touch. Click touch refers to a device that differentiates
        // between a simple "tap" and a hard press that causes the touch screen to click.
        bool is_active = data.touch_1.is_active != 0;

        float x = 0;
        float y = 0;

        if (is_active && status->touch_calibration) {
            u16 min_x = status->touch_calibration->min_x;
            u16 max_x = status->touch_calibration->max_x;
            u16 min_y = status->touch_calibration->min_y;
            u16 max_y = status->touch_calibration->max_y;

            x = (std::clamp(static_cast<u16>(data.touch_1.x), min_x, max_x) - min_x) /
                static_cast<float>(max_x - min_x);
            y = (std::clamp(static_cast<u16>(data.touch_1.y), min_y, max_y) - min_y) /
                static_cast<float>(max_y - min_y);
        }

        status->touch_status = {x, y, is_active};
    }
}

} // namespace InputCommon::CemuhookUDP
