#include <iostream>

// --- Key Test ---
// This line tests if our build system has correctly configured the
// include paths to the llama.cpp submodule. If this line compiles,
// the include part of the setup is working.
#include "llama.h"

int main() {
    std::cout << "UMA Serve daemon starting up..." << std::endl;
    std::cout << "---------------------------------" << std::endl;

    // --- Another Key Test ---
    // This function call tests if our executable has been correctly
    // "linked" against the compiled llama.cpp library. If the program
    // runs and prints system information, the linking is successful.
    //
    // llama_print_system_info() is a great test function because it
    // prints detailed hardware info, confirming that llama.cpp is aware
    // of the machine's capabilities (like Apple Metal).
    std::cout << "Printing system info from llama.cpp:" << std::endl;
    std::cout << llama_print_system_info() << std::endl;
    std::cout << "---------------------------------" << std::endl;

    std::cout << "Test successful! UMA Serve can correctly call llama.cpp." << std::endl;
    std::cout << "Shutting down." << std::endl;

    return 0;
}
