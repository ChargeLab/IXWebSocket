#include "IXTest.h"
#include "ixwebsocket/IXWebSocketMessageType.h"
#include <catch_amalgamated.hpp>
#include <ixwebsocket/IXUrlParser.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <chrono>
#include <memory>

using namespace ix;

static std::atomic<bool> client_connected {false};
static std::atomic<bool> client_closed {false};

TEST_CASE("SendTimeout")
{
    SECTION("Test send timeout kicking in")
    {
        // Create a server with a one second send timeout
        int port = getFreePort();
        std::unique_ptr<ix::WebSocketServer> server = std::unique_ptr<ix::WebSocketServer>(
            new ix::WebSocketServer(port,
                                    "127.0.0.1",
                                    SocketServer::kDefaultTcpBacklog,
                                    SocketServer::kDefaultMaxConnections,
                                    WebSocketServer::kDefaultHandShakeTimeoutSecs,
                                    AF_INET,
                                    /*pingIntervalSeconds=*/5,
                                    /*sendTimeoutSeconds=*/1));

        auto res = server->listen();
        REQUIRE(res.first);

        server->setOnConnectionCallback(
            [](std::weak_ptr<WebSocket> wws, std::shared_ptr<ConnectionState> /*cs*/) -> void
            {
                TLogger() << "Client connected!";
                auto ws = wws.lock();
                client_connected = true;

                // When the client sends a message, send it 50k messages back
                // to quickly fill up the socket buffer and run into a send
                // timeout.
                ws->setOnMessageCallback(
                    [ws](const WebSocketMessagePtr& wsmptr)
                    {
                        if (wsmptr->type == WebSocketMessageType::Message)
                        {
                            auto i = 0;
                            while (++i < 50000)
                            {
                                auto r = ws->sendText("SPAM SPAM SPAM SPAM SPAM SPAM!");
                                if (!r.success)
                                {
                                    ws->close();
                                    break;
                                }
                            }
                        }
                        else if (wsmptr->type == WebSocketMessageType::Close)
                        {
                            TLogger()
                                << "SERVER: Client connection closed:" << wsmptr->closeInfo.reason;
                            client_closed = true;
                        }
                    });
            });

        std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/";
        ix::WebSocket client;
        client.setUrl(url);

        client.setOnMessageCallback(
            [&client](const ix::WebSocketMessagePtr& msg)
            {
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    TLogger() << "CLIENT: Open";
                    client.sendText("Hello");
                }
                else if (msg->type == ix::WebSocketMessageType::Close)
                {
                    TLogger() << "CLIENT: Close";
                }
                else if (msg->type == ix::WebSocketMessageType::Message)
                {
                    auto r = client.sendText("Hello, again!");

                    // Block the client thread after sending a message
                    // to make the socket buffers run full.
                    if (r.success) msleep(1000);
                }
            });

        server->start();
        client.start();

        // Wait for client to connect and be closed again.
        while (!client_connected || !client_closed)
        {
            msleep(10);
        }

        server->stop();
    }
}

TEST_CASE("NonBlockingSend")
{
    SECTION("Server sends do not block the caller when the client stops reading")
    {
        std::atomic<bool> hello_received {false};
        std::atomic<bool> server_closed {false};
        std::atomic<bool> client_stalled {true};
        std::mutex close_reason_mutex;
        std::string close_reason;
        std::weak_ptr<WebSocket> server_side;

        // Create a server with a three second send timeout and non-blocking sends
        int port = getFreePort();
        std::unique_ptr<ix::WebSocketServer> server = std::unique_ptr<ix::WebSocketServer>(
            new ix::WebSocketServer(port,
                                    "127.0.0.1",
                                    SocketServer::kDefaultTcpBacklog,
                                    SocketServer::kDefaultMaxConnections,
                                    WebSocketServer::kDefaultHandShakeTimeoutSecs,
                                    AF_INET,
                                    /*pingIntervalSeconds=*/5,
                                    /*sendTimeoutSeconds=*/3));
        server->disableBlockingSend();

        auto res = server->listen();
        REQUIRE(res.first);

        server->setOnConnectionCallback(
            [&](std::weak_ptr<WebSocket> wws, std::shared_ptr<ConnectionState> /*cs*/) -> void
            {
                server_side = wws;
                auto ws = wws.lock();
                ws->setOnMessageCallback(
                    [&](const WebSocketMessagePtr& wsmptr)
                    {
                        if (wsmptr->type == WebSocketMessageType::Message)
                        {
                            hello_received = true;
                        }
                        else if (wsmptr->type == WebSocketMessageType::Close)
                        {
                            {
                                std::lock_guard<std::mutex> lock(close_reason_mutex);
                                close_reason = wsmptr->closeInfo.reason;
                            }
                            server_closed = true;
                        }
                    });
            });

        std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/";
        ix::WebSocket client;
        client.setUrl(url);
        client.disableAutomaticReconnection();

        client.setOnMessageCallback(
            [&client, &client_stalled](const ix::WebSocketMessagePtr& msg)
            {
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    client.sendText("Hello");
                }
                else if (msg->type == ix::WebSocketMessageType::Message)
                {
                    // Block the client thread so it stops reading and the
                    // socket buffers run full.
                    while (client_stalled)
                    {
                        msleep(10);
                    }
                }
            });

        // Release the client on every exit path, so teardown does not hang
        // when an assertion fails.
        struct ReleaseClient
        {
            std::atomic<bool>& stalled;
            ~ReleaseClient()
            {
                stalled = false;
            }
        } release_client {client_stalled};

        server->start();
        client.start();

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!hello_received && std::chrono::steady_clock::now() < deadline)
        {
            msleep(10);
        }
        REQUIRE(hello_received);

        auto ws = server_side.lock();
        REQUIRE(ws != nullptr);

        // Send ~12 MB from this thread, as an application thread would. That is
        // more than the kernel socket buffers hold, so with blocking sends this
        // loop would stall for the 3 second send timeout.
        const std::string payload(64 * 1024, 'x');
        size_t max_buffered = 0;
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; ++i)
        {
            auto r = ws->sendText(payload);
            if (!r.success) break;
            max_buffered = std::max(max_buffered, ws->bufferedAmount());
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        TLogger() << "200 sends took " << elapsed_ms << " ms, max buffered "
                  << max_buffered << " bytes";

        REQUIRE(elapsed_ms < 2000);
        REQUIRE(max_buffered > 0);

        // The connection's own thread flushes the buffer and still enforces the
        // send timeout.
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!server_closed && std::chrono::steady_clock::now() < deadline)
        {
            msleep(10);
        }
        REQUIRE(server_closed);
        {
            std::lock_guard<std::mutex> lock(close_reason_mutex);
            REQUIRE(close_reason == "Send timeout");
        }

        ws.reset();
        client_stalled = false;
        client.stop();
        server->stop();
    }
}
