/*  */

/*                      FTP access functions            HTFTP.h
**                      ====================
*/


#ifndef HTFTP_H
#define HTFTP_H

#include "HTAnchor.h"
#include "HTStream.h"

/*      Retrieve File from Server
**      -------------------------
**
** On exit,
**      returns         Socket number for file if good.
**                      <0 if bad.
*/
extern int HTFTPLoad
PARAMS
((
  CONST char *          name,
  HTParentAnchor *      anchor,
  HTFormat              format_out,
  HTStream*             sink
));

/*      Close socket opened for reading a file, and get final message
**      -------------------------------------------------------------
**
*/
/*extern int HTFTP_close_file PARAMS ((int soc)); */


/*      Return Host Name
**      ----------------
*/
extern CONST char * HTHostName
NOPARAMS;

#endif
/*

    */
