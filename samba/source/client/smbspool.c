/* 
   Unix SMB/CIFS implementation.
   SMB backend for the Common UNIX Printing System ("CUPS")
   Copyright 1999 by Easy Software Products
   Copyright Andrew Tridgell 1994-1998
   Copyright Andrew Bartlett 2002
   Copyright Rodrigo Fernandez-Vizarra 2005 
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "includes.h"

/*
   Starting with CUPS 1.3, Kerberos support is provided by cupsd including
   the forwarding of user credentials via the authenticated session between
   user and server and the KRB5CCNAME environment variable which will point
   to a temporary file or an in-memory representation depending on the version
   of Kerberos you use.  As a result, all of the ticket code that used to
   live here has been removed, and we depend on the user session (if you
   run smbspool by hand) or cupsd to provide the necessary Kerberos info.

   Also, the AUTH_USERNAME and AUTH_PASSWORD environment variables provide
   for per-job authentication for non-Kerberized printing.  We use those
   if there is no username and password specified in the device URI.

   Finally, if we have an authentication failure we return exit code 2
   which tells CUPS to hold the job for authentication and bug the user
   to get the necessary credentials.
*/

#define MAX_RETRY_CONNECT        3


/*
 * Globals...
 */

extern BOOL		in_client;	/* Boolean for client library */


/*
 * Local functions...
 */

static void		list_devices(void);
static struct cli_state *smb_complete_connection(const char *, const char *,int , const char *, const char *, const char *, const char *, int, int *need_auth);
static struct cli_state	*smb_connect(const char *, const char *, int, const char *, const char *, const char *, const char *, int *need_auth);
static int		smb_print(struct cli_state *, char *, FILE *);
static char *		uri_unescape_alloc(const char *);


/*
 * 'main()' - Main entry for SMB backend.
 */

 int				/* O - Exit status */
 main(int  argc,			/* I - Number of command-line arguments */
     char *argv[])		/* I - Command-line arguments */
{
  int		i;		/* Looping var */
  int		copies;		/* Number of copies */
  int 		port;		/* Port number */
  char		uri[1024],	/* URI */
		*sep,		/* Pointer to separator */
		*tmp, *tmp2,	/* Temp pointers to do escaping */
		*password;	/* Password */
  char		*username,	/* Username */
		*server,	/* Server name */
		*printer;	/* Printer name */
  const char	*workgroup;	/* Workgroup */
  FILE		*fp;		/* File to print */
  int		status=0;		/* Status of LPD job */
  struct cli_state *cli;	/* SMB interface */
  char null_str[1];
  int tries = 0;
  const char *dev_uri;
  int need_auth;

  null_str[0] = '\0';

  /* we expect the URI in argv[0]. Detect the case where it is in argv[1] and cope */
  if (argc > 2 && strncmp(argv[0],"smb://", 6) && !strncmp(argv[1],"smb://", 6)) {
	  argv++;
	  argc--;
  }

  if (argc == 1)
  {
   /*
    * NEW!  In CUPS 1.1 the backends are run with no arguments to list the
    *       available devices.  These can be devices served by this backend
    *       or any other backends (i.e. you can have an SNMP backend that
    *       is only used to enumerate the available network printers... :)
    */

    list_devices();
    return (0);
  }

  if (argc < 6 || argc > 7)
  {
    fprintf(stderr, "Usage: %s [DEVICE_URI] job-id user title copies options [file]\n",
            argv[0]);
    fputs("       The DEVICE_URI environment variable can also contain the\n", stderr);
    fputs("       destination printer:\n", stderr);
    fputs("\n", stderr);
    fputs("           smb://[username:password@][workgroup/]server[:port]/printer\n", stderr);
    return (1);
  }

 /*
  * If we have 7 arguments, print the file named on the command-line.
  * Otherwise, print data from stdin...
  */


  if (argc == 6)
  {
   /*
    * Print from Copy stdin to a temporary file...
    */

    fp     = stdin;
    copies = 1;
  }
  else if ((fp = fopen(argv[6], "rb")) == NULL)
  {
    perror("ERROR: Unable to open print file");
    return (1);
  }
  else
    copies = atoi(argv[4]);

 /*
  * Find the URI...
  */

  dev_uri = getenv("DEVICE_URI");
  if (dev_uri)
    strncpy(uri, dev_uri, sizeof(uri) - 1);
  else if (strncmp(argv[0], "smb://", 6) == 0)
    strncpy(uri, argv[0], sizeof(uri) - 1);
  else
  {
    fputs("ERROR: No device URI found in DEVICE_URI environment variable or argv[0] !\n", stderr);
    return (1);
  }

  uri[sizeof(uri) - 1] = '\0';

 /*
  * Extract the destination from the URI...
  */

  if ((sep = strrchr_m(uri, '@')) != NULL)
  {
    tmp = uri + 6;
    *sep++ = '\0';

    /* username is in tmp */

    server = sep;

   /*
    * Extract password as needed...
    */

    if ((tmp2 = strchr_m(tmp, ':')) != NULL) {
      *tmp2++ = '\0';
      password = uri_unescape_alloc(tmp2);
    } else {
      password = null_str;
    }
    username = uri_unescape_alloc(tmp);
  }
  else
  {
    if ((username = getenv("AUTH_USERNAME")) == NULL)
      username = null_str;

    if ((password = getenv("AUTH_PASSWORD")) == NULL)
      password = null_str;

    server = uri + 6;
  }

  tmp = server;

  if ((sep = strchr_m(tmp, '/')) == NULL)
  {
    fputs("ERROR: Bad URI - need printer name!\n", stderr);
    return (1);
  }

  *sep++ = '\0';
  tmp2 = sep;

  if ((sep = strchr_m(tmp2, '/')) != NULL)
  {
   /*
    * Convert to smb://[username:password@]workgroup/server/printer...
    */

    *sep++ = '\0';

    workgroup = uri_unescape_alloc(tmp);
    server    = uri_unescape_alloc(tmp2);
    printer   = uri_unescape_alloc(sep);
  }
  else {
    workgroup = NULL;
    server = uri_unescape_alloc(tmp);
    printer = uri_unescape_alloc(tmp2);
  }
  
  if ((sep = strrchr_m(server, ':')) != NULL)
  {
    *sep++ = '\0';

    port=atoi(sep);
  }
  else 
  	port=0;
	
 
 /*
  * Setup the SAMBA server state...
  */

  setup_logging("smbspool", True);

  in_client = True;   /* Make sure that we tell lp_load we are */

  load_case_tables();

  if (!lp_load(dyn_CONFIGFILE, True, False, False, True))
  {
    fprintf(stderr, "ERROR: Can't load %s - run testparm to debug it\n", dyn_CONFIGFILE);
    return (1);
  }

  if (workgroup == NULL)
    workgroup = lp_workgroup();

  load_interfaces();

  do
  {
    if ((cli = smb_connect(workgroup, server, port, printer, username, password, argv[2], &need_auth)) == NULL)
    {
      if (need_auth)
      {
        fputs("ATTR: auth-info-required=username,password\n", stderr);
	exit(2);
      }
      else if (getenv("CLASS") == NULL)
      {
        fprintf(stderr, "ERROR: Unable to connect to CIFS host, will retry in 60 seconds...\n");
        sleep(60);
        tries++;
      }
      else
      {
        fprintf(stderr, "ERROR: Unable to connect to CIFS host, trying next printer...\n");
        return (1);
      }
    }
  }
  while ((cli == NULL) && (tries < MAX_RETRY_CONNECT));

  if (cli == NULL) {
        fprintf(stderr, "ERROR: Unable to connect to CIFS host after (tried %d times)\n", tries);
        return (1);
  }

 /*
  * Now that we are connected to the server, ignore SIGTERM so that we
  * can finish out any page data the driver sends (e.g. to eject the
  * current page...  Only ignore SIGTERM if we are printing data from
  * stdin (otherwise you can't cancel raw jobs...)
  */

  if (argc < 7)
    CatchSignal(SIGTERM, SIG_IGN);

 /*
  * Queue the job...
  */

  for (i = 0; i < copies; i ++)
    if ((status = smb_print(cli, argv[3] /* title */, fp)) != 0)
      break;

  cli_shutdown(cli);

 /*
  * Return the queue status...
  */

  return (status);
}


/*
 * 'list_devices()' - List the available printers seen on the network...
 */

static void
list_devices(void)
{
 /*
  * Eventually, search the local workgroup for available hosts and printers.
  */

  puts("network smb \"Unknown\" \"Windows Printer via SAMBA\"");
}


static struct cli_state 
*smb_complete_connection(const char *myname,
            const char *server,
            int port,
            const char *username, 
            const char *password, 
            const char *workgroup, 
            const char *share,
            int flags,
	    int *need_auth)
{
  struct cli_state  *cli;    /* New connection */    
  NTSTATUS nt_status;
  int i;
  static const NTSTATUS auth_errors[] =
  { /* List of NTSTATUS errors that are considered authentication errors */
    NT_STATUS_ACCESS_DENIED, NT_STATUS_ACCESS_VIOLATION,
    NT_STATUS_SHARING_VIOLATION, NT_STATUS_PRIVILEGE_NOT_HELD,
    NT_STATUS_INVALID_ACCOUNT_NAME, NT_STATUS_NO_SUCH_USER,
    NT_STATUS_WRONG_PASSWORD, NT_STATUS_LOGON_FAILURE,
    NT_STATUS_ACCOUNT_RESTRICTION, NT_STATUS_INVALID_LOGON_HOURS,
    NT_STATUS_PASSWORD_EXPIRED, NT_STATUS_ACCOUNT_DISABLED
  };

  /* Start the SMB connection */
  *need_auth = 0;
  nt_status = cli_start_connection( &cli, myname, server, NULL, port, 
                                    Undefined, flags, NULL);
  if (!NT_STATUS_IS_OK(nt_status)) 
  {
    fprintf(stderr,"ERROR: Connection failed: %s\n", nt_errstr(nt_status));
    return NULL;      
  }
    
  /* We pretty much guarentee password must be valid or a pointer
     to a 0 char. */
  if (!password) {
    *need_auth = 1;
    return NULL;
  }
  
  nt_status = cli_session_setup(cli, username,
				password, strlen(password)+1,
				password, strlen(password)+1,
				workgroup);
  if (!NT_STATUS_IS_OK(nt_status))
  {
    fprintf(stderr,"ERROR: Session setup failed: %s\n", nt_errstr(nt_status));

    for (i = 0; i < (int)(sizeof(auth_errors) / sizeof(auth_errors[0])); i ++)
      if (NT_STATUS_V(nt_status) == NT_STATUS_V(auth_errors[i]))
      {
	*need_auth = 1;
	break;
      }

    cli_shutdown(cli);

    return NULL;
  }
    
  if (!cli_send_tconX(cli, share, "?????", password, strlen(password)+1)) 
  {
    fprintf(stderr, "ERROR: Tree connect failed (%s)\n", cli_errstr(cli));
    nt_status = cli_nt_error(cli);

    for (i = 0; i < (int)(sizeof(auth_errors) / sizeof(auth_errors[0])); i ++)
      if (NT_STATUS_V(nt_status) == NT_STATUS_V(auth_errors[i]))
      {
	*need_auth = 1;
	break;
      }

    cli_shutdown(cli);

    return NULL;
  }
    
  return cli;
}

/*
 * 'smb_connect()' - Return a connection to a server.
 */

static struct cli_state *		/* O - SMB connection */
smb_connect(const char *workgroup,	/* I - Workgroup */
            const char *server,		/* I - Server */
            const int port,		/* I - Port */
            const char *share,		/* I - Printer */
            const char *username,	/* I - Username */
            const char *password,	/* I - Password */
            const char *jobusername,	/* I - User who issued the print job */
            int *need_auth)		/* O - Need authentication? */
{
  struct cli_state  *cli;    /* New connection */
  pstring    myname;    /* Client name */
  struct passwd *pwd;

 /*
  * Get the names and addresses of the client and server...
  */

  get_myname(myname);  

  /* See if we have a username first.  This is for backwards compatible 
     behavior with 3.0.14a */

  if (username &&  *username && !getenv("KRB5CCNAME"))
  {
      cli = smb_complete_connection(myname, server, port, username, 
                                    password, workgroup, share, 0, need_auth);
      if (cli) 
        return cli;
  }
  
  /* 
   * Try to use the user kerberos credentials (if any) to authenticate
   */
  cli = smb_complete_connection(myname, server, port, jobusername, "", 
                                workgroup, share, 
                                CLI_FULL_CONNECTION_USE_KERBEROS, need_auth);

  if (cli ) { return cli; }

  /* give a chance for a passwordless NTLMSSP session setup */

  pwd = getpwuid(geteuid());
  if (pwd == NULL) {
     return NULL;
  }

  cli = smb_complete_connection(myname, server, port, pwd->pw_name, "", 
                                workgroup, share, 0, need_auth);

  if (cli) { return cli; }

  /*
   * last try. Use anonymous authentication
   */

  cli = smb_complete_connection(myname, server, port, "", "", 
                                workgroup, share, 0, need_auth);
  /*
   * Return the new connection...
   */
  
  return (cli);
}


/*
 * 'smb_print()' - Queue a job for printing using the SMB protocol.
 */

static int				/* O - 0 = success, non-0 = failure */
smb_print(struct cli_state *cli,	/* I - SMB connection */
          char             *title,	/* I - Title/job name */
          FILE             *fp)		/* I - File to print */
{
  int	fnum;		/* File number */
  int	nbytes,		/* Number of bytes read */
	tbytes;		/* Total bytes read */
  char	buffer[8192],	/* Buffer for copy */
	*ptr;		/* Pointer into tile */


 /*
  * Sanitize the title...
  */

  for (ptr = title; *ptr; ptr ++)
    if (!isalnum((int)*ptr) && !isspace((int)*ptr))
      *ptr = '_';

 /*
  * Open the printer device...
  */

  if ((fnum = cli_open(cli, title, O_RDWR | O_CREAT | O_TRUNC, DENY_NONE)) == -1)
  {
    fprintf(stderr, "ERROR: %s opening remote spool %s\n",
            cli_errstr(cli), title);
    return (1);
  }

 /*
  * Copy the file to the printer...
  */

  if (fp != stdin)
    rewind(fp);

  tbytes = 0;

  while ((nbytes = fread(buffer, 1, sizeof(buffer), fp)) > 0)
  {
    if (cli_write(cli, fnum, 0, buffer, tbytes, nbytes) != nbytes)
    {
      fprintf(stderr, "ERROR: Error writing spool: %s\n", cli_errstr(cli));
      break;
    }

    tbytes += nbytes;
  } 

  if (!cli_close(cli, fnum))
  {
    fprintf(stderr, "ERROR: %s closing remote spool %s\n",
            cli_errstr(cli), title);
    return (1);
  }
  else
    return (0);
}

static char *uri_unescape_alloc(const char *uritok)
{
	char *ret;

	ret = (char *)SMB_STRDUP(uritok);
	if (!ret) return NULL;

	rfc1738_unescape(ret);
	return ret;
}
