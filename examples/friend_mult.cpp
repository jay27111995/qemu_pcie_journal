// Friend operator example: int * Box
#include <iostream>
using namespace std;

class Box {
    int value;
public:
    Box(int v) : value(v) {}
    
    // 5 * box  →  left side is int, not Box, so needs friend
    friend Box operator*(int n, const Box& b) {
        return Box(n * b.value);
    }
    
    void print() { cout << value << endl; }
};

int main() {
    Box b(10);
    
    Box result = 5 * b;  // int on left, Box on right
    
    result.print();  // prints: 50
    
    return 0;
}
