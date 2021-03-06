#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <thread>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/sendfile.h>

#include "sockets.h"
#include "headers.h"
#include "debug.h"
#include "concurrentQueue.h"

#define LINE_LENGTH 5000
#define BUFFER_LENGTH 10000

ConcurrentQueue q;
std::mutex qMutex;

void generateDirectory(std::string rootPath, char URI[], char buf[]) {
	dprintf("%s\n", URI);
	strcpy(buf, "<html>\r\n<body>\r\n<h1>Index of ");
	strcpy(buf, URI);
	strcpy(buf, "</h1>\r\n<ul>\r\n");

	DIR* d;
	d = opendir(URI);
	struct dirent* dir;
	if (d) {
		while ((dir = readdir(d))) {
			if (strcmp(dir->d_name, ".") == 0) {
				continue;
			}
			strcat(buf, "<li><a href=\"");
			strcat(buf, URI+rootPath.length());
			strcat(buf, "/");
			strcat(buf, dir->d_name);
			strcat(buf, "\"> ");
			strcat(buf, dir->d_name);
			strcat(buf, "</a></li>\r\n");
		}

		closedir(d);
	}

	strcat(buf, "</ul>\r\n</body>\r\n</html>");
}

void serve(int tid, std::string rootPath) {
	while(true) {
		qMutex.lock();
		int sock = q.pop();
		dprintf("serving %d\n", sock);

		bool isDirectory = false;
		char dirBuf[BUFFER_LENGTH];

		char cmd[LINE_LENGTH];
		char URI[LINE_LENGTH];
		memset(URI, 0, LINE_LENGTH);
		double version;

		//read the request line
		char* line = GetLine(sock);
		sscanf(line, "%s %s HTTP/%lf", cmd, URI, &version);

		//normalize root path and URI
		std::string root = rootPath.c_str();
		dprintf("\nBEFORE\nroot: %s\nURI: %s", root.c_str(), URI);

		if (root[0] != '/' && root[0] != '.') {
			root = "./" + root;
		}

		if (root.length() > 1 && root[root.length()-1] == '/') {
			root = root.substr(0, root.length()-1);
		}

		if (URI[strlen(URI)-1] == '/') {
			URI[strlen(URI)-1] = '\0';
		}
		dprintf("\nAFTER\nroot: %s\nURI: %s", root.c_str(), URI);

		std::string tempURI = root + std::string(URI);
		strcpy(URI, tempURI.c_str());
		dprintf("\n----------------\nCommand: %s\nURI: %s\nHTTP version: %lf\n\n", cmd, URI, version);

		//read the rest of the request headers
		while (strlen(line) > 0) {
			dprintf("%s\n", line);
			line = GetLine(sock);
		}

		//write the status line
		int status = 200;
		char message[LINE_LENGTH];
		sprintf(message, "OK");
		long int fileLength;
		struct stat filestat;
		int statError = stat(URI, &filestat);

		if (!statError && S_ISREG(filestat.st_mode)) {
			dprintf("Found a file\n");
		}
		else if (!statError && S_ISDIR(filestat.st_mode)) {
			dprintf("Found a directory\n");
			//check for index.html page
			char indexURI[LINE_LENGTH];
			sprintf(indexURI, "%s/index.html", URI);

			stat(indexURI, &filestat);
			if (S_ISREG(filestat.st_mode)) { //use the index.html page
				sprintf(URI, "%s", indexURI);
				dprintf("%s\n", URI);
			}
			else { //output a directory listing
				dprintf("%s is not an index file\n", indexURI);
				isDirectory = true;
				generateDirectory(rootPath, URI, dirBuf);
				fileLength = strlen(dirBuf);
			}
		}
		else {
			status = 404;
			sprintf(message, "Not Found");
			sprintf(URI, "404.html");
		}

		char statusLine[LINE_LENGTH];
		sprintf(statusLine, "HTTP/1.1 %d %s\r\n", status, message);
		write(sock, statusLine, strlen(statusLine));
		dprintf("%s", statusLine);

		//write the Content-Type line
		dprintf("file type of: %s\n", URI);
		const char* fileType = strrchr(URI, '.');
		const char* type;
		dprintf("%s\n", fileType);
		if (!fileType || strlen(fileType) <= 1 || strchr(fileType, '/')) { //directory
			type = "text/html";
		}
		else if (strcmp(fileType, ".html") == 0) {
			type = "text/html";
		}
		else if (strcmp(fileType, ".txt") == 0) {
			type = "text/plain";
		}
		else if (strcmp(fileType, ".jpg") == 0) {
			type = "image/jpg";
		}
		else if (strcmp(fileType, ".gif") == 0) {
			type = "image/gif";
		}
		else {
			type = "text/plain";
			dprintf("Unknown file type\n");
		}
		char contentTypeLine[LINE_LENGTH];
		sprintf(contentTypeLine, "Content-Type: %s\r\n", type);
		write(sock, contentTypeLine, strlen(contentTypeLine));
		dprintf("%s", contentTypeLine);

		//write the Content-Length line
		stat(URI, &filestat);
		if (fopen(URI, "r") && S_ISREG(filestat.st_mode)) {
			fileLength = filestat.st_size;
		}
		char contentLength[LINE_LENGTH];
		sprintf(contentLength, "Content-Length: %ld\r\n", fileLength);
		write(sock, contentLength, strlen(contentLength));
		dprintf("%s", contentLength);

		//write the rest of the response headers
		char other[] = "Connection: close\r\n";
		write(sock, other, strlen(other));
		dprintf("%s", other);

		//write a blank line
		char blank[] = "\r\n";
		write(sock, blank, strlen(blank));
		dprintf("%s\n", blank);

		//write the file
		if (!isDirectory) {
			dprintf("about to read from %s\n", URI);
			FILE* file = fopen(URI, "r");
			int fd = fileno(file);
			off_t* offset;

			if(sendfile(sock, fd, NULL, fileLength) < 0) {
				dprintf("Error writing file to socket: %d", errno);
			}
			fclose(file);
		}
		else {
			dprintf("about to read directory listing\n%s", dirBuf);
			write(sock, dirBuf, strlen(dirBuf));
		}

		shutdown(sock, SHUT_RDWR);
		close(sock);
	}
}

int main(int argc, char* argv[]) {
	//lock signal semaphore
	qMutex.lock();

	//parse arguments
	if (argc < 4) {
		printf("Usage: server <port number> <num threads> <dir>\n");
		exit(0);
	}

	int port = atoi(argv[1]);
	int numThreads = atoi(argv[2]);
	char* dir = argv[3];

	//create thread pool
	std::vector<std::thread> workers;
	for (int i = 0; i < numThreads; i++) {
		workers.push_back(std::thread(serve, i, dir));
	}

	dprintf("create a socket\n");
	int socket = sockCreate();

	dprintf("convert address\n");
	struct sockaddr_in address = createAddress(port);

	dprintf("bind socket to address\n");
	sockBind(socket, (struct sockaddr*)&address);

	dprintf("listen to socket\n");
	sockListen(socket);

	dprintf("allow socket to be reused\n");
	int optval = 1;
	setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	dprintf("accept connections to socket\n");
	int newSocket;
	while (newSocket = sockAccept(socket, (struct sockaddr*)&address)) {
		dprintf("Adding new task: %d\n", newSocket);
		q.push(newSocket);
		qMutex.unlock();
	}

	dprintf("all done. Cleaning up thread pool\n");
	for (auto &thread: workers) {
		thread.join();
	}

}