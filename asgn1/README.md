# httpserver
The goal of this assignment was to implement a single-threaded HTTP server that will respond to simple GET and PUT commands to read and write 'files' named by 10-character
ASCII names. The server will persistently store files in a directory on the server, so it can be restarted or otherwise run on a directory that already has files.

## Usage

Type "make" into the terminal to compile and link httpserver.cpp.

Run the executable with "./httpserver <hostname/ip address> [port]"
