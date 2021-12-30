# httpserver
The goals for this assignment were to modify the HTTP server in any previous assignment and implement backups and recovery. 
The HTTP server should have the ability to store a backup of all the files in the server and recover to an earlier backup. 

## Usage

Type "make" into terminal to compile and link httpserver.cpp.

Run the executable with "./httpserver <hostname/ip address> [port]"

We based our code mainly off asgn1, so the HTTP server is not multi-threaded and there is no redundancy.
