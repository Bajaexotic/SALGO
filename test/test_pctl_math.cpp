#include <iostream>
#include <cmath>

int main() {
    double val = 0.0;
    double med = 1.6;
    double m = 0.74;
    
    // From percentileRank()
    double z = (val - med) / (m * 1.4826);
    double p = 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
    double pctl = p * 100.0;
    
    std::cout << "val=" << val << " med=" << med << " MAD=" << m << std::endl;
    std::cout << "z = (" << val << " - " << med << ") / (" << m << " * 1.4826) = " << z << std::endl;
    std::cout << "erf(" << z << " / sqrt(2)) = erf(" << (z / std::sqrt(2.0)) << ") = " << std::erf(z / std::sqrt(2.0)) << std::endl;
    std::cout << "p = 0.5 * (1 + " << std::erf(z / std::sqrt(2.0)) << ") = " << p << std::endl;
    std::cout << "pctl = " << pctl << "%" << std::endl;
    
    std::cout << "\nExpected: ~7.7%, Got from log: 25%" << std::endl;
    std::cout << "If MAD < 1e-9, fallback would return: " << ((val >= med) ? 75.0 : 25.0) << "%" << std::endl;
    
    return 0;
}
