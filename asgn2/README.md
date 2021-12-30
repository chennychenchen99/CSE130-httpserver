# httpserver
The goals for this assignment were to modify the single-threaded HTTP server in asgn1 and implement multi-threading and redundancy. 
Synchronization techniques had to be used to service multiple requests at once and to ensure that different copies of the same file are consistent.

## Usage

Type "make" into the terminal to compile and link httpserver.cpp.

Run the executable with "./httpserver <hostname/ip address> [port] [-N num of threads] [-r]"
