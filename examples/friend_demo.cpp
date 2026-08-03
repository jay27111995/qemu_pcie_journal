// Friend function example
#include <iostream>
using namespace std;

class Box {
    int secret;  // private - no one can access

public:
    Box(int s) : secret(s) {}

    // This says: "peek() function is my friend, let it see my secrets"
    friend void peek(const Box& b);
};

// peek() is NOT inside the class, but can still see 'secret'
void peek(const Box& b) {
    cout << "The secret is: " << b.secret << endl;
}

int main() {
    Box mybox(42);
    
    // cout << mybox.secret;  // ERROR - secret is private
    
    peek(mybox);  // OK - peek is a friend
    
    return 0;
}
