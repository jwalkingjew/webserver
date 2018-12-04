
const char * usage =
"                                                               \n"
"myhttpd [-f|-t|-p] [<port>]:                                   \n"
"    -f: Create a new process for each request                  \n"
"    -t: Create a new thread for each request                   \n"
"    -p: pool of threads                                        \n"
"                                                               \n"
;

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <string>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <boost/algorithm/string/predicate.hpp>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <fstream>

int QueueLength = 5;
const char * secret = "pie";
const char * sec1 = "/pie";
const char * sec2 = "/pie/";
int ascending = 1;
int numReqs = 0;
struct timespec tstart={0,0}, tend={0,0};
double minTime = -1;
double maxTime = -1;
char lastPath[1024] = {0};
char minPath[1024] = {0};
char maxPath[1024] = {0};
// Processes time request
void processRequest( int socket );
void processRequestThread( int socket );
void pool( int socket );
const char * read_dir( DIR* d, int socket, char *path, char *cwd );
static int myCompare (const void * a, const void * b);
void sort(const char *arr[], int n); 
pthread_mutex_t mt;
pthread_mutexattr_t mattr;

extern "C" void zombies(int sig);

int main( int argc, char ** argv )
{
  clock_gettime(CLOCK_MONOTONIC, &tstart);
  int port;
  int mode;
  mode = 0; // default mode
  port = 1080; // default port
  
  if ( argc < 1 || argc > 3 ) {
    // Print usage if not enough, or too many arguments
    fprintf( stderr, "%s\n", usage );
    exit( -1 );
  } else if ( argc == 2 || argc == 3 ) {
	if ( argv[1][0] == '-' ) {
		// Find appropriate mode
		if ( argv[1][1] == 'f' ) {
			mode = 1;
		} else if ( argv[1][1] == 't' ) {
			mode = 2;
		} else if ( argv[1][1] == 'p' ) {
			mode = 3;
		} else {
			fprintf( stderr, "%s\n", usage );
			exit( -1 );
		}
	}
	// Set appropriate port number
	if ( argv[1][0] != '-' && argc == 2 ) {
		port = atoi( argv[1] );
	} else if ( argc == 2 ) {
		port = 1080;
	} else if ( argc == 3 ) {
		port = atoi( argv[2] );
	} else {
		fprintf( stderr, "%s\n", usage );
		exit( -1 );
	}
  }
  // make sure port is within allowed values
  if ( port < 1024 || port > 65536 ) {
	fprintf( stderr, "%s\n", usage );
	exit( -1 );
  }

  // Catch zombie processes
  struct sigaction sa;
  sa.sa_handler = zombies;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
    
  if (sigaction(SIGCHLD, &sa, NULL)) {
      perror("sigaction");
      exit(-1);
  }

  // Set the IP address and port for this server
  struct sockaddr_in serverIPAddress; 
  memset( &serverIPAddress, 0, sizeof(serverIPAddress) );
  serverIPAddress.sin_family = AF_INET;
  serverIPAddress.sin_addr.s_addr = INADDR_ANY;
  serverIPAddress.sin_port = htons((u_short) port);
  
  // Allocate a socket
  int masterSocket =  socket(PF_INET, SOCK_STREAM, 0);
  if ( masterSocket < 0) {
    perror("socket");
    exit( -1 );
  }

  // Set socket options to reuse port. Otherwise we will
  // have to wait about 2 minutes before reusing the sae port number
  int optval = 1; 
  int err = setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR, 
		       (char *) &optval, sizeof( int ) );
   
  // Bind the socket to the IP address and port
  int error = bind( masterSocket,
		    (struct sockaddr *)&serverIPAddress,
		    sizeof(serverIPAddress) );
  if ( error ) {
    perror("bind");
    exit( -1 );
  }
  
  // Put socket in listening mode and set the 
  // size of the queue of unprocessed connections
  error = listen( masterSocket, QueueLength);
  if ( error ) {
    perror("listen");
    exit( -1 );
  }

  struct timespec tmpstart={0,0}, tmpend={0,0};
  double diff;
  if ( mode == 3 ) {
	// Pool of Threads Mode
	// allows for locking and unlocking
      	pthread_mutexattr_init(&mattr);
      	pthread_mutex_init(&mt, &mattr);

      	pthread_t tid[QueueLength];
      	pthread_attr_t attr;
      	pthread_attr_init(&attr);
      	pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);

	int i;
	i = 0;
      	while ( i < QueueLength ) {
          	pthread_create(&tid[i], &attr, (void * (*)(void *))pool, (void *)masterSocket);
      		i++;
	}

	pthread_join(tid[0], NULL);
  } else {

	while ( 1 ) {

		// Accept incoming connections
		struct sockaddr_in clientIPAddress;
		int alen = sizeof( clientIPAddress );
		int slaveSocket = accept( masterSocket,
			      (struct sockaddr *)&clientIPAddress,
			      (socklen_t*)&alen);

		if ( slaveSocket < 0 ) {
			perror( "accept" );
			exit( -1 );
		}

		if ( mode == 0 ) {
			clock_gettime(CLOCK_MONOTONIC, &tmpstart);
			//DEFAULT ITERATIVE MODE
			processRequest( slaveSocket );
			clock_gettime(CLOCK_MONOTONIC, &tmpend);
			shutdown( slaveSocket, 1 );
			close( slaveSocket );

			diff = ((double)tmpend.tv_sec + 1.0e-9*tmpend.tv_nsec) - ((double)tmpstart.tv_sec + 1.0e-9*tmpstart.tv_nsec);

		} else if ( mode == 1 ) {

			clock_gettime(CLOCK_MONOTONIC, &tmpstart);
			//CREATE NEW PROCESS FOR EACH REQUEST	
			pid_t slave;
			slave = fork();

			if ( slave == 0 ) {
				processRequest( slaveSocket );
			//	shutdown( slaveSocket, 1 ); // should this be shutdown or close?
				exit( 1 );
			}
			close( slaveSocket );
			clock_gettime(CLOCK_MONOTONIC, &tmpend);

			diff = ((double)tmpend.tv_sec + 1.0e-9*tmpend.tv_nsec) - ((double)tmpstart.tv_sec + 1.0e-9*tmpstart.tv_nsec);
		
		} else if ( mode == 2 ) {

			//CREATE NEW THREAD FOR EACH REQUEST
			pthread_t tid;
			pthread_attr_t attr;

			pthread_attr_init(&attr);
			pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);

			pthread_create(&tid, &attr, (void * (*)(void *)) processRequestThread, (void *) slaveSocket);


		} else {

			fprintf( stderr, "%s\n", usage );
			exit( -1 );

		}
		if (diff > 0) {
			if (diff > maxTime || maxTime == -1) {
				maxTime = diff;
//				maxPath = strdup(lastPath);
				strcpy(maxPath, lastPath);
			}
			if (diff < minTime || minTime == -1) {
				minTime = diff;
//				minPath = strdup(lastPath);
				strcpy(minPath, lastPath);
			}
		}
  	}
  }
}

void pool( int socket ) {
// function to implement pool mode
  	struct timespec tmpstart={0,0}, tmpend={0,0};
  	double diff;
	while( true ) {
		pthread_mutex_lock(&mt);
		struct sockaddr_in clientIPAddress;
		int alen = sizeof( clientIPAddress );
		int slaveSocket = accept( socket,
				(struct sockaddr *)&clientIPAddress,
				(socklen_t*)&alen );

		pthread_mutex_unlock(&mt);

		clock_gettime(CLOCK_MONOTONIC, &tmpstart);
		processRequest( slaveSocket );
		clock_gettime(CLOCK_MONOTONIC, &tmpend);
		shutdown( slaveSocket, 1 );
		close( slaveSocket );

		diff = ((double)tmpend.tv_sec + 1.0e-9*tmpend.tv_nsec) - ((double)tmpstart.tv_sec + 1.0e-9*tmpstart.tv_nsec);
		if (diff > 0) {
			if (diff > maxTime || maxTime == -1) {
				maxTime = diff;
//				maxPath = strdup(lastPath);
				strcpy(maxPath, lastPath);
			}
			if (diff < minTime || minTime == -1) {
				minTime = diff;
//				minPath = strdup(lastPath);
				strcpy(minPath, lastPath);
			}
		}
	}
}

void processRequestThread( int socket ) {
// function to implement thread mode
	
  	struct timespec tmpstart={0,0}, tmpend={0,0};
  	double diff;
	clock_gettime(CLOCK_MONOTONIC, &tmpstart);
	processRequest( socket );
	clock_gettime(CLOCK_MONOTONIC, &tmpend);
	//shutdown( socket ); //??
	close( socket );

	diff = ((double)tmpend.tv_sec + 1.0e-9*tmpend.tv_nsec) - ((double)tmpstart.tv_sec + 1.0e-9*tmpstart.tv_nsec);
	if (diff > 0) {
		if (diff > maxTime || maxTime == -1) {
			maxTime = diff;
//			maxPath = strdup(lastPath);
			strcpy(maxPath, lastPath);
		}
		if (diff < minTime || minTime == -1) {
			minTime = diff;
//			minPath = strdup(lastPath);
			strcpy(minPath, lastPath);
		}
	}
}

void processRequest( int socket ) {
  // Define variables
  const int maxSize = 1024;
  char request[ maxSize + 1 ];
  int reqLength;
  int n;
  char path [ maxSize + 1 ] = {0}; // path to requested document
  unsigned char newChar; // current character
  unsigned char lastChar; // last character
  char prev[4] = {0}; // previous 4 characters
  int get; // flag to confirm request contains GET
  int key; // flag to confirm that request contains special key
  int doc; // flag to know when to start recording doc path

  // initialize variables to zero
  reqLength = 0;
  newChar = 0;
  lastChar = 0;
  get = 0;
  key = 0;
  doc = 0;

  while (read( socket, &newChar, sizeof(newChar))) {
    if ( newChar == ' ' ) {
	if ( get == 0 ) {
		get = 1;
	} else if (doc == 0) {
		doc = 1;
		request[ reqLength ] = 0;
		if (reqLength == 4) {
			if (true) {
				strcpy(path, "/");
			} else {
				printf(request);
//				exit(1);
			}
		} else if (true) {
			char * req = request;
			//req = req + strlen(sec1);
			strcpy(path, req);
		} else if (boost::algorithm::starts_with(request, "/fav")) {
			strcpy(path, request);
		} else if (boost::algorithm::starts_with(request, "/cgi-bin")) {
			strcpy(path, request);
		} else {
			printf(request);
//			exit(1);
		}
		request[ reqLength ] = newChar;
		reqLength++;
	}
    } else if ( newChar == '\n' && lastChar == '\r'  && prev[2] == '\n' && prev[1] == '\r') {
	break;
    } else {
	lastChar = newChar;
	if (get == 1) {
    		request[ reqLength ] = newChar;
		reqLength++;
	}
    }
    prev[0] = prev[1];
    prev[1] = prev[2];
    prev[2] = prev[3];
    prev[3] = newChar;
  }
  // Add null character at the end of the string
  request[ reqLength ] = 0;
  numReqs++;
  char reqNew[maxSize] = {0};
  strcpy(reqNew, "GET ");
  strcat(reqNew, request);
  strcpy(request, reqNew);
  FILE * logFile = fopen("/home/u91/pmantel/cs252/webserver/http-root-dir/htdocs/logs", "a");
  if (logFile != NULL) {
	fprintf(logFile, "%s",  request);
	fclose(logFile);
  }
  printf("request: %s\n", request);
  strcpy(lastPath, path);
  // for debugging purposes
  printf( "path=%s\n", path );

  char cwd[ 256 ] = {0};
  getcwd(cwd, sizeof(cwd));

  int icons = (boost::algorithm::starts_with(path, "/icons"));
  int htdocs = (boost::algorithm::starts_with(path, "/htdocs"));
  int cgibin = (boost::algorithm::starts_with(path, "/cgi-bin"));
  int stats = (boost::algorithm::starts_with(path, "/stats"));
  int logs = (boost::algorithm::starts_with(path, "/logs"));

  int data = (boost::algorithm::starts_with(path, "/add-data"));
  int viewdata = (boost::algorithm::starts_with(path, "/data"));

  if (data) {
	FILE * datanew = fopen("/home/u91/pmantel/cs252/webserver/http-root-dir/htdocs/newData", "w");
	char header[] = "Date [dd-mm-yy]\tTime [UTC]\tX-Accleration [m/s^2]\tY-Acceleration [m/s^2]\tZ-Acceleration [m/s^2]\tX-Gyro [deg/sec]\tY-Gyro [deg/sec]\tZ-Gyro [deg/sec]\tLatitude [deg N]\tLongitude[deg E]\tAltitude [m]\tSpeed [m/s]\tSatellite Num\tTemperature [F]\tRPM\n";
//	char header[] = "data1\tdata2\tdata3\tdata4\n";
	if (datanew != NULL) {
		fprintf(datanew, "%s", header);
		fclose(datanew);
	}
  }

  if (icons || htdocs || cgibin || data) {
        strcat(cwd, "/http-root-dir");
        strcat(cwd, path);
  } else {
        if (!strcmp(path, "/"))
        {
            	strcpy(path, "/index.html");
		strcat(cwd, "/http-root-dir/htdocs");
        	strcat(cwd, path);
        } else {
        	strcat(cwd, "/http-root-dir/htdocs");
        	strcat(cwd, path);
	}	
  }
  // CGI-BIN implementation
  setenv("REQUEST_METHOD", "GET", 1);
  char * env = strstr(cwd, "?");
  char var[maxSize] = {0};
  char * val;
  char newEnv[maxSize];
  if (env != NULL && data) {
	cwd[strlen(cwd) - strlen(env)] = '\0';
	env++;

	strcpy(var, env);
	printf("Data: %s\n", var);
  	
	FILE * datanew = fopen("/home/u91/pmantel/cs252/webserver/http-root-dir/htdocs/newData", "a");
	if (datanew != NULL) {
		char * curr = var;
		char * comma = strstr(var, ",");
		if (comma != NULL) {
			curr[strlen(curr) - strlen(comma)] = '\0';
			comma++;
		}
		fprintf(datanew, "%s", curr);
		while (comma != NULL) {
			curr = comma;
			comma = strstr(curr, ",");
			if (comma != NULL) {
				curr[strlen(curr) - strlen(comma)] = '\0';
				comma++;
			}
			fprintf(datanew, "\t%s", curr);
		}
		fprintf(datanew, "\n");
	}
	
	int i = 0;
	char line[0x1000];
	FILE * olddata = fopen("/home/u91/pmantel/cs252/webserver/http-root-dir/htdocs/oldData", "r");
    	if( olddata == NULL) {
        	printf("Old data file failed to open\n");
    	} 
//	else {
		while (fgets(line, sizeof(line), olddata) != NULL) {
			if (i != 0){
				fprintf(datanew, "%s", line);
			}
			i++;
		}
		fclose(olddata);
		fclose(datanew);
		remove("/home/u91/pmantel/cs252/webserver/http-root-dir/htdocs/oldData");
		rename("/home/u91/pmantel/cs252/webserver/http-root-dir/htdocs/newData", "/home/u91/pmantel/cs252/webserver/http-root-dir/htdocs/oldData");
//	}
} if (env != NULL && cgibin) {
	cwd[strlen(cwd) - strlen(env)] = '\0';
	env++;
	
	strcpy(var, env);
	val = strstr(env, "=");
	var[strlen(var)-strlen(val)] = '\0';
	val++;
	env = strstr(val, "&");
	if (env != NULL) {
		env++;
		strcpy(newEnv, env);
		val[strlen(val)-(strlen(newEnv)+1)] = '\0';
	}
	setenv(var, val, 1);
	while (env != NULL) {
		strcpy(var, newEnv);
		val = strstr(newEnv, "=");
		var[strlen(var)-strlen(val)] = '\0';
		val++;
		env = strstr(val, "&");
		if (env != NULL) {
			env++;
			strcpy(newEnv, env);
			val[strlen(val)-(strlen(newEnv)+1)] = '\0';
		}
		setenv(var, val, 1);
		
	}
  }
    
  if (strstr(path, "..") != NULL) {
	int len;
	int expLen;
        char expanded[maxSize + 1] = {0};
        char *exp = realpath(path, expanded);

        len = strlen(cwd) + 14;
	expLen = strlen(expanded);
        if (exp != NULL) {
            if (expLen >= len) {
                strcpy(cwd, expanded);
	    } else {	
		perror("realpath");
		exit( 0 );
	    }
	} else {
		perror("realpath");
		exit( 0 );
	}
  }
    
  printf("path=%s\n", path);
  printf("cwd=%s\n", cwd);

  // Determine requested content type
  char contentType[maxSize + 1] = {0};
    
  int html = (boost::algorithm::ends_with(path, ".html") || boost::algorithm::ends_with(path, ".html/"));
  int jpg = (boost::algorithm::ends_with(path, ".jpg") || boost::algorithm::ends_with(path, ".jpg/"));
  int gif = (boost::algorithm::ends_with(path, ".gif") || boost::algorithm::ends_with(path, ".gif/"));
  int ico = (boost::algorithm::ends_with(path, ".ico") || boost::algorithm::ends_with(path, ".ico/"));
  int svg = (boost::algorithm::ends_with(path, ".svg") || boost::algorithm::ends_with(path, ".svg/"));
  int png = (boost::algorithm::ends_with(path, ".png") || boost::algorithm::ends_with(path, ".png/"));
  int xbm = (boost::algorithm::ends_with(path, ".xbm") || boost::algorithm::ends_with(path, ".xbm/"));
  DIR * d = NULL;
  int dir = 0;

  if (html || viewdata) {
      strcpy(contentType, "text/html");
  } else if (jpg) {
      strcpy(contentType, "image/jpg");
  } else if (gif) {
      strcpy(contentType, "image/gif");
      if (boost::algorithm::ends_with(path, ".gif/")) {
	path[strlen(path)-1] = '\0';
	cwd[strlen(cwd)-1]='\0';
      }
  } else if (ico) {
//      strcpy(contentType, "image/x-icon");
      strcpy(contentType, "image/vnd.microsoft.icon");
  } else if (svg) {
      strcpy(contentType, "image/svg+xml");
  } else if (png) {
      strcpy(contentType, "image/png");
  } else if (xbm) {
      strcpy(contentType, "image/x-xbitmap");
  } else {
      d = opendir(cwd);
      if (d != NULL) {
	dir = 1;
	strcpy(contentType, "text/html");
      } else {
      	strcpy(contentType, "text/plain");
	if (boost::algorithm::ends_with(path, "/")) {
		path[strlen(path)-1] = '\0';
		cwd[strlen(cwd)-1]='\0';
	}
      }
  }

  if (data || viewdata) {
	strcpy(cwd, "/home/u91/pmantel/cs252/webserver/http-root-dir/htdocs/oldData");
  }

  FILE *f;
  f = NULL;
  if (dir != 1 && !cgibin && !stats) {
  	f = fopen(cwd, "rb");
  	if (f == NULL) {
		f = fopen(cwd, "r");
	}
  } else if (cgibin) {
	f = popen(cwd, "r");
  } else if (logs) {
	f = fopen(cwd, "r");
  }
  if (dir == 1) {
	if (!boost::algorithm::ends_with(path, "/")) {
		strcat(path, "/");
	}
	if (!boost::algorithm::ends_with(cwd, "/")) {
		strcat(cwd, "/");
	}
  }

  if (stats) { 
      write(socket, "HTTP/1.1 200 OK\r\n", 17);
      write(socket, "Server: CS 252 Lab5\r\n", 21);
      write(socket, "Content-Type: ", 14);
      write(socket, contentType, strlen(contentType));
      write(socket, "\r\n\r\n", 4);
      write(socket, "Student Creator: Preston Mantel\r\n", 33);
      clock_gettime(CLOCK_MONOTONIC, &tend);
      double elapsed = ((double)tend.tv_sec + 1.0e-9*tend.tv_nsec) - 
           ((double)tstart.tv_sec + 1.0e-9*tstart.tv_nsec);
      char buf[500] = {0};
      sprintf(buf, "%.5f", ((double)tend.tv_sec + 1.0e-9*tend.tv_nsec) - 
           ((double)tstart.tv_sec + 1.0e-9*tstart.tv_nsec));
      write(socket, "Time Elapsed: ", 14);
      write(socket, buf, strlen(buf));
      write(socket, " seconds\r\n", 10);
      sprintf(buf, "%d", numReqs);
      write(socket, "Number of Requests: ", 20);
      write(socket, buf, strlen(buf));
      write(socket, "\r\n", 2);
      sprintf(buf, "%.5f", minTime);
      write(socket, "Minimum Service Time: ", 22);
      write(socket, buf, strlen(buf));
      write(socket, " seconds\r\n", 10);
      write(socket, "Path with Min Service time: ", 28);
      write(socket, minPath, strlen(minPath));
      write(socket, "\r\n", 2);
      sprintf(buf, "%.5f", maxTime);
      write(socket, "Maximum Service Time: ", 22);
      write(socket, buf, strlen(buf));
      write(socket, " seconds\r\n", 10);
      write(socket, "Path with Max Service time: ", 28);
      write(socket, maxPath, strlen(maxPath));
      write(socket, "\r\n", 2);
  } else if (f != NULL || d != NULL) {
      write(socket, "HTTP/1.1 200 OK\r\n", 17);
      write(socket, "Server: CS 252 Lab5\r\n", 21);
      if (!cgibin) {
     	write(socket, "Content-Type: ", 14);
     	write(socket, contentType, strlen(contentType));
      	write(socket, "\r\n\r\n", 4);
      }
      if (viewdata) {
	write(socket, "<!DOCTYPE HTML>\r\n", 17);
	write(socket, "<html>\r\n", 8);
	write(socket, "<head>\r\n", 8);
	write(socket, "<title>", 7);
	write(socket, "REALTIME PUP VEHICLE DATA", 25);
	write(socket, "</title>\r\n", 10);
	write(socket, "</head>\r\n", 9);
	write(socket, "<body>\r\n", 8);
	write(socket, "<h1>", 4);
	write(socket, "REALTIME PUP VEHICLE DATA", 25);
	write(socket, "</h1>\r\n", 7);
	write(socket, "<table id=\"myTable\">\r\n", 22);
	
	FILE * data = fopen("/home/u91/pmantel/cs252/webserver/http-root-dir/htdocs/oldData", "r");
	int i = 0;
	char line[0x1000];
    	if( data == NULL) {
        	printf("Old data file failed to open\n");
    	}
	while (fgets(line, sizeof(line), data) != NULL) {
		if (true){
			
			char * curr = line;
			curr[strlen(curr)-1] = '\0';
			char * comma = strstr(curr, "\t");
			if (comma != NULL) {
				curr[strlen(curr) - strlen(comma)] = '\0';
				comma++;
			}
			printf("%s\n", curr);
			write(socket, "<tr>", 4);
			write(socket, "<th>", 4);
			write(socket, curr, strlen(curr));
			write(socket, "</th>", 5);
			while (comma != NULL) {
				curr = comma;
				comma = strstr(curr, "\t");
				if (comma != NULL) {
					curr[strlen(curr) - strlen(comma)] = '\0';
					comma++;
				}
				printf("%s\n", curr);
				write(socket, "<th>", 4);
				write(socket, curr, strlen(curr));
				write(socket, "</th>", 5);
			}
			write(socket, "</tr>\r\n", 7);
		}
		i++;
	}
	
	write(socket, "</table></body></html>", 22);
	write(socket, "\r\n\r\n", 4);
	fclose(data);
      }	
      // debugging purposes
      printf("content= %s\n", contentType);        
      if (dir) {
	char parentdir[strlen(cwd)]={0};
	strcpy(parentdir, path);
	parentdir[strlen(parentdir) - 1] = '\0';
	while (parentdir[strlen(parentdir) - 1] != '/') {
		parentdir[strlen(parentdir) - 1] = '\0';
	}
	write(socket, "<!DOCTYPE HTML>\r\n", 17);
	write(socket, "<html>\r\n", 8);
	write(socket, "<head>\r\n", 8);
	write(socket, "<title>Index of ", 16);
	write(socket, cwd, strlen(cwd));
	write(socket, "</title>\r\n", 10);
	write(socket, "</head>\r\n", 9);
	write(socket, "<body>\r\n", 8);
	write(socket, "<h1>Index of ", 13);
	write(socket, cwd, strlen(cwd));
	write(socket, "</h1>\r\n", 7);
	write(socket, "<table id=\"myTable\">\r\n", 22);
	write(socket, "<tr><th valign=\"top\"><img src=\"/pie/icons/blank.xbm\" alt=\"[ICO]\"></th>", 70);
	write(socket, "<th><button onclick=\"sortName(1)\">Name</button></th>", 52);
	write(socket, "<th><button onclick=\"sortName(2)\">Last Modified</button></th>", 62);
	write(socket, "<th><button onclick=\"sortName(3)\">Size</button></th>", 52);
	write(socket, "<th><button onclick=\"sortName(4)\">Description</button></th>", 59);
	write(socket, "</tr>\r\n", 7);
	write(socket, "<tr><th colspan=\"5\"><hr></th></tr>\r\n", 37);
	write(socket, "<tr><td valign=\"top\">", 22);
	write(socket, "<img src=\"/pie/icons/back.gif\" alt=\"[PARENTDIR]\"></td>", 55);
	write(socket, "<td><a ", 7);
	write(socket, "href=\"", 6);
	write(socket, "/pie", 4);
	write(socket, parentdir, strlen(parentdir));
	write(socket, "\">Parent Directory</a></td>", 27);
	write(socket, "<td>&nbsp;</td><td align=\"right\">  - </td><td>&nbsp;</td></tr>", 62);
	read_dir(d, socket, path, cwd);
	write(socket, "</table>\r\n", 10);
	write(socket, "<script>\r\n", 10);
	write(socket, "var asc = true;\r\n", 17); 
	write(socket, "function sortName(col) {\r\n", 26);
	write(socket, "\tasc = !asc;\r\n", 14);
  	write(socket, "\tvar table, rows, switching, i, x, y, shouldSwitch;\r\n", 53);
  	write(socket, "\ttable = document.getElementById(\"myTable\");\r\n", 46);
  	write(socket, "\tswitching = true;\r\n", 20);
  	write(socket, "\twhile (switching) {\r\n", 22);
    	write(socket, "\t\tswitching = false;\r\n", 22);
    	write(socket, "\t\trows = table.rows;\r\n", 22);
    	write(socket, "\t\tfor (i = 3; i < (rows.length - 1); i++) {\r\n", 45);
      	write(socket, "\t\t\tshouldSwitch = false;\r\n", 26);
      	write(socket, "\t\t\tx = rows[i].getElementsByTagName(\"TD\")[col];\r\n", 49);
      	write(socket, "\t\t\ty = rows[i + 1].getElementsByTagName(\"TD\")[col];\r\n", 53);
      	write(socket, "\t\t\tif (", 7);
	write(socket, "asc", 3);
	write(socket, ") {\r\n", 5);
	write(socket, "\t\t\t\tif (x.innerHTML.toLowerCase() > y.innerHTML.toLowerCase()) {\r\n", 66);
        write(socket, "\t\t\t\t\tshouldSwitch = true;\r\n", 27);
        write(socket, "\t\t\t\t\tbreak;\r\n", 13);
      	write(socket, "\t\t\t\t}\r\n", 7);
	write(socket, "\t\t\t} else {\r\n", 13);
	write(socket, "\t\t\t\tif (x.innerHTML.toLowerCase() < y.innerHTML.toLowerCase()) {\r\n", 66);
        write(socket, "\t\t\t\t\tshouldSwitch = true;\r\n", 27);
        write(socket, "\t\t\t\t\tbreak;\r\n", 13);
      	write(socket, "\t\t\t\t}\r\n", 7);
	write(socket, "\t\t\t}\r\n", 6);
    	write(socket, "\t\t}\r\n", 5);
    	write(socket, "\t\tif (shouldSwitch) {\r\n", 23);
      	write(socket, "\t\trows[i].parentNode.insertBefore(rows[i + 1], rows[i]);\r\n", 58);
      	write(socket, "\t\tswitching = true;\r\n", 21);
    	write(socket, "\t\t}\r\n", 5);
  	write(socket, "\t}\r\n", 4);
	write(socket, "}\r\n", 3);
	write(socket, "</script>\r\n", 11);
	write(socket, "</body></html>\r\n", 16);
	write(socket, "\r\n\r\n", 4);
	
      } else if (!viewdata) {
      	int count;
      	char ch;
      	while (count = read(fileno(f), &ch, sizeof(ch)) > 0) {
           if (write(socket, &ch, sizeof(ch)) != count) {
              	perror("write");
	   }
      	}
      	fclose(f);
      }
  } else {
      const char * notFound = "File not found";
        
      write(socket, "HTTP/1.1 404 File Not Found\r\n", 29);
      write(socket, "Server: CS 252 Lab5\r\n", 21);
      write(socket, "Content-type: text/html\r\n\r\n", 27);
      write(socket, notFound, strlen(notFound));
  }

}
extern "C" void zombies(int sig) {
    wait3(0, 0, NULL);
    while(waitpid(-1, NULL, WNOHANG) > 0);
}

const char * read_dir(DIR *d, int socket, char * path, char * cwd) {	
	char table[500] = {0};
	struct dirent *dp;
	char icon[15] = {0};
	char newpath[500] = {0};
	while ((dp=readdir(d)) != NULL) {
		strcpy(table, dp->d_name);
		strcpy(newpath, cwd);
		strcat(newpath, table);

		struct stat attrib;
		stat(newpath, &attrib);
		char buf[100] = {0};
		char time[50];
		strftime(time, 50, "%Y-%m-%d %H:%M", localtime(&attrib.st_mtime));
		long int size = attrib.st_size;
		sprintf(buf, "%ld\n", size);

		int html = (boost::algorithm::ends_with(table, ".html") || boost::algorithm::ends_with(table, ".html/"));
		int jpg = (boost::algorithm::ends_with(table, ".jpg") || boost::algorithm::ends_with(table, ".jpg/"));
		int gif = (boost::algorithm::ends_with(table, ".gif") || boost::algorithm::ends_with(table, ".gif/"));
		int ico = (boost::algorithm::ends_with(table, ".ico") || boost::algorithm::ends_with(table, ".ico/"));
		int svg = (boost::algorithm::ends_with(table, ".svg") || boost::algorithm::ends_with(table, ".svg/"));
		int png = (boost::algorithm::ends_with(table, ".png") || boost::algorithm::ends_with(table, ".png/"));
		int xbm = (boost::algorithm::ends_with(table, ".xbm") || boost::algorithm::ends_with(table, ".xbm/"));
		int cc = (boost::algorithm::ends_with(table, ".cc") || boost::algorithm::ends_with(table, ".cc/"));

		int dir = 0;
		DIR *dn = opendir(newpath);
		if (dn != NULL) {
			dir = 1;
			closedir(dn);
		}
		
		if (jpg || gif || ico || svg || png || xbm ) {
			strcpy(icon, "image.gif");
		} else if (cc || html) {
			strcpy(icon, "text.gif");
		} else if (dir) {
			strcpy(icon, "unknown.gif");
		} else {
			strcpy(icon, "unknown.gif");
		}
		printf("path= %s\n\n", path);	
		write(socket, "<tr><td valign=\"top\"><img src=\"", 31);
		write(socket, "/pie/icons/", 11);
		write(socket, icon, strlen(icon));
		write(socket, "\" alt=\"[   ]\"></td><td><a href=\"", 32);
		write(socket, "/pie", 4);
		write(socket, path, strlen(path));
		write(socket, table, strlen(table));
		write(socket, "/", 1);
		write(socket, "\">", 2);
		write(socket, table, strlen(table));
		write(socket, "</a></td><td align=\"right\">", 27);
		write(socket, time, strlen(time));
		write(socket, "  </td><td align=\"right\">", 25);
		write(socket, buf, strlen(buf));
		write(socket, " </td><td>&nbsp;</td></tr>", 26);

		printf("table= %s\n", table);
	}
	closedir(d);
	return NULL;
}

static int myCompare (const void * a, const void * b) 
{ 
    return strcmp (*(const char **) a, *(const char **) b); 
} 
  
void sort(const char *arr[], int n) 
{ 
    qsort (arr, n, sizeof (const char *), myCompare); 
} 
