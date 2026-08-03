// unique_ptr example - no manual memory management needed
#include <iostream>
#include <memory>
using namespace std;

class Buffer {
    unique_ptr<int[]> data;
    int size;
public:
    Buffer(int n) : data(make_unique<int[]>(n)), size(n) {
        cout << "Buffer created, size=" << size << endl;
        for (int i = 0; i < size; i++) data[i] = i * 10;
    }
    
    // No destructor needed - unique_ptr handles it
    // No copy constructor needed - unique_ptr disables copying
    
    // Move constructor (transfer ownership)
    Buffer(Buffer&& other) noexcept 
        : data(move(other.data)), size(other.size) {
        other.size = 0;
        cout << "Buffer moved" << endl;
    }
    
    void print() {
        cout << "[ ";
        for (int i = 0; i < size; i++) cout << data[i] << " ";
        cout << "]" << endl;
    }
};

int main() {
    Buffer a(5);
    a.print();  // [ 0 10 20 30 40 ]
    
    // Buffer b = a;  // ERROR - can't copy unique_ptr
    
    Buffer b = move(a);  // OK - transfer ownership
    b.print();  // [ 0 10 20 30 40 ]
    
    cout << "End of main" << endl;
    // b's destructor runs automatically - no memory leak
    return 0;
}
