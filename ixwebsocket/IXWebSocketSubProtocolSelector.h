/*
 *  IXWebSocketSubProtocolSelector.h
 *  Copyright (c) 2026 ChargeLab. All rights reserved.
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ix
{
    //
    // Server side sub-protocol negotiation (RFC 6455 section 4.2.2).
    // Called during the handshake with the sub-protocols offered by the client
    // in the Sec-WebSocket-Protocol header, in the client's order of preference.
    // Return the one to accept, or an empty string to accept none.
    // A value that is not one of the offered sub-protocols is ignored.
    //
    using SubProtocolSelector =
        std::function<std::string(const std::vector<std::string>& offeredSubProtocols)>;
} // namespace ix
