#include <iostream>
#include "engine/order.hpp"

int main() {
    Order order;
    std::cout << "sizeof(Order) = " << sizeof(order) << '\n';
    std::cout << "matching engine: build OK" << '\n';

    return 0;
}