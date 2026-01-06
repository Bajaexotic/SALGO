#include "../AMT_DomEvents.h"
#include "../AMT_Liquidity.h"
#include <iostream>

int main() {
    std::cout << "Testing includes...\n";
    AMT::DomValueLocation loc = AMT::DomValueLocation::AT_POC;
    std::cout << "Location: " << static_cast<int>(loc) << "\n";
    return 0;
}
