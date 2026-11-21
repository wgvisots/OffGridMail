// Winlink secure login: the answer to a gateway's ";PQ: <challenge>" line.
#pragma once

#include <string>
#include <string_view>

namespace ogm {

// Returns the 8-digit response sent back as ";PR: <response>".  The password
// is used exactly as given (Winlink passwords are case-sensitive).
std::string secureLoginResponse(std::string_view challenge, std::string_view password);

}  // namespace ogm
