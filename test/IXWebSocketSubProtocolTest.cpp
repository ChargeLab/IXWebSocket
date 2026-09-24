/*
 *  IXWebSocketServerTest.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2019 Machine Zone. All rights reserved.
 */

#include "IXTest.h"
#include <catch_amalgamated.hpp>
#include <iostream>
#include <ixwebsocket/IXSocket.h>
#include <ixwebsocket/IXSocketFactory.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

using namespace ix;

bool startServer(ix::WebSocketServer& server, std::string& subProtocols)
{
    server.setOnClientMessageCallback(
        [&server, &subProtocols](std::shared_ptr<ConnectionState> connectionState,
                                 WebSocket& webSocket,
                                 const ix::WebSocketMessagePtr& msg) {
            auto remoteIp = connectionState->getRemoteIp();
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                TLogger() << "New connection";
                TLogger() << "remote ip: " << remoteIp;
                TLogger() << "id: " << connectionState->getId();
                TLogger() << "Uri: " << msg->openInfo.uri;
                TLogger() << "Headers:";
                for (auto it : msg->openInfo.headers)
                {
                    TLogger() << it.first << ": " << it.second;
                }

                subProtocols = msg->openInfo.headers["Sec-WebSocket-Protocol"];
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                log("Closed connection");
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                for (auto&& client : server.getClients())
                {
                    if (client.get() != &webSocket)
                    {
                        client->sendBinary(msg->str);
                    }
                }
            }
        });

    auto res = server.listen();
    if (!res.first)
    {
        log(res.second);
        return false;
    }

    server.start();
    return true;
}

TEST_CASE("subprotocol", "[websocket_subprotocol]")
{
    SECTION("Connect to the server")
    {
        int port = getFreePort();
        ix::WebSocketServer server(port);

        std::string subProtocols;
        startServer(server, subProtocols);

        std::atomic<bool> connected(false);
        ix::WebSocket webSocket;
        webSocket.setOnMessageCallback([&connected](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                connected = true;
                log("Client connected");
            }
        });

        webSocket.addSubProtocol("json");
        webSocket.addSubProtocol("msgpack");

        std::string url;
        std::stringstream ss;
        ss << "ws://127.0.0.1:" << port;
        url = ss.str();

        webSocket.setUrl(url);
        webSocket.start();

        // Give us 3 seconds to connect
        int attempts = 0;
        while (!connected)
        {
            REQUIRE(attempts++ < 300);
            ix::msleep(10);
        }

        webSocket.stop();
        server.stop();

        REQUIRE(subProtocols == "json,msgpack");
    }
}

TEST_CASE("subprotocol_selector", "[websocket_subprotocol]")
{
    auto negotiate = [](const std::string& selection,
                        std::string& serverProtocol,
                        std::string& clientProtocol) {
        int port = getFreePort();
        ix::WebSocketServer server(port);

        std::vector<std::string> offered;
        server.setSubProtocolSelector(
            [&offered, selection](const std::vector<std::string>& subProtocols) {
                offered = subProtocols;
                return selection;
            });
        server.setOnClientMessageCallback(
            [&serverProtocol](std::shared_ptr<ConnectionState>,
                              WebSocket&,
                              const ix::WebSocketMessagePtr& msg) {
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    serverProtocol = msg->openInfo.protocol;
                }
            });
        REQUIRE(server.listenAndStart());

        std::atomic<bool> connected(false);
        ix::WebSocket webSocket;
        webSocket.setOnMessageCallback(
            [&connected, &clientProtocol](const ix::WebSocketMessagePtr& msg) {
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    clientProtocol = msg->openInfo.protocol;
                    connected = true;
                }
            });
        webSocket.addSubProtocol("ocpp2.0.1");
        webSocket.addSubProtocol(" ocpp1.6");
        webSocket.setUrl("ws://127.0.0.1:" + std::to_string(port));
        webSocket.start();

        int attempts = 0;
        while (!connected)
        {
            REQUIRE(attempts++ < 300);
            ix::msleep(10);
        }

        webSocket.stop();
        server.stop();

        std::vector<std::string> expected = {"ocpp2.0.1", "ocpp1.6"};
        REQUIRE(offered == expected);
    };

    SECTION("Selected sub-protocol is echoed back")
    {
        std::string serverProtocol, clientProtocol;
        negotiate("ocpp1.6", serverProtocol, clientProtocol);
        REQUIRE(serverProtocol == "ocpp1.6");
        REQUIRE(clientProtocol == "ocpp1.6");
    }

    SECTION("Sub-protocol not offered by the client is ignored")
    {
        std::string serverProtocol = "unset", clientProtocol = "unset";
        negotiate("ocpp1.5", serverProtocol, clientProtocol);
        REQUIRE(serverProtocol.empty());
        REQUIRE(clientProtocol.empty());
    }
}
