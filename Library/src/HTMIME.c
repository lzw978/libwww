/*								       HTMIME.c
**	MIME MESSAGE PARSE
**
**	(c) COPYRIGHT MIT 1995.
**	Please first read the full copyright statement in the file COPYRIGH.
**	@(#) $Id$
**
**	This is RFC 1341-specific code.
**	The input stream pushed into this parser is assumed to be
**	stripped on CRs, ie lines end with LF, not CR LF.
**	(It is easy to change this except for the body part where
**	conversion can be slow.)
**
** History:
**	   Feb 92	Written Tim Berners-Lee, CERN
**	 8 Jul 94  FM	Insulate free() from _free structure element.
**	14 Mar 95  HFN	Now using anchor for storing data. No more `\n',
**			static buffers etc.
*/

/* Library include files */
#include "sysdep.h"
#include "WWWUtil.h"
#include "WWWCore.h"
#include "HTReqMan.h"
#include "HTNetMan.h"
#include "HTHeader.h"
#include "HTWWWStr.h"
#include "HTMIME.h"					 /* Implemented here */

#define MIME_HASH_SIZE 101

/*		MIME Object
**		-----------
*/
struct _HTStream {
    const HTStreamClass *	isa;
    HTRequest *			request;
    HTNet *			net;
    HTParentAnchor *		anchor;
    HTStream *			target;
    HTFormat			target_format;
    HTChunk *			token;
    HTChunk *			value;
    int				hash;
    HTEOLState			EOLstate;
    BOOL			transparent;
    BOOL			head_only;
    BOOL			nntp;
    BOOL			footer;
    BOOL			haveToken;
};

/* ------------------------------------------------------------------------- */

PRIVATE int pumpData (HTStream * me)
{
    HTRequest * request = me->request;
    HTParentAnchor * anchor = me->anchor;
    me->transparent = YES;		  /* Pump rest of data right through */

    /* If this request us a source in PostWeb then pause here */
    if (me->head_only || HTRequest_isSource(request)) return HT_PAUSE;

    /* If HEAD method then we just stop here */
    if (me->footer || request->method == METHOD_HEAD) return HT_LOADED;

    /*
    ** Handle any Content Type
    ** News server almost never send content type or content length
    */
    {
	HTFormat format = HTAnchor_format(anchor);
	if (format != WWW_UNKNOWN || me->nntp) {
	    if (STREAM_TRACE) HTTrace("Building.... C-T stack from %s to %s\n",
				      HTAtom_name(format),
				      HTAtom_name(me->target_format));
	    me->target = HTStreamStack(format, me->target_format,
				       me->target, request, YES);
	}
    }

    /* Handle any Content Encoding */
    {
	HTList * cc = HTAnchor_encoding(anchor);
	if (cc) {
	    if (STREAM_TRACE) HTTrace("Building.... C-E stack\n");
	    me->target = HTContentDecodingStack(cc, me->target, request, NULL);
	}
    }

    /* Handle any Transfer encoding */
    {
	HTEncoding transfer = HTAnchor_transfer(anchor);
	if (!HTFormat_isUnityTransfer(transfer)) {
	    if (STREAM_TRACE) HTTrace("Building.... C-T-E stack\n");
	    me->target = HTTransferCodingStack(transfer, me->target,
					       request, NULL, NO);
	}
    }
    return HT_OK;
}

/*
**	Header is terminated by CRCR, LFLF, CRLFLF, CRLFCRLF
**	Folding is either of CF LWS, LF LWS, CRLF LWS
*/
PRIVATE int HTMIME_put_block (HTStream * me, const char * b, int l)
{
    const char * start = b;
    const char * end = start;
    const char * value = me->value->size ? b : NULL;
    long cl;
    int status;
    /*    enum {Line_CHAR, Line_END, Line_FOLD, Line_LINE} line = Line_CHAR; */

    while (!me->transparent) {
	if (me->EOLstate == EOL_FCR) {
	    if (*b == CR)				    /* End of header */
	        me->EOLstate = EOL_END;
	    else if (*b == LF)			   	     /* CRLF */
		me->EOLstate = EOL_FLF;
	    else if (WHITE(*b))				   /* Folding: CR SP */
	        me->EOLstate = EOL_FOLD;
	    else						 /* New line */
	        me->EOLstate = EOL_LINE;
	} else if (me->EOLstate == EOL_FLF) {
	    if (*b == CR)				/* LF CR or CR LF CR */
		me->EOLstate = EOL_SCR;
	    else if (*b == LF)				    /* End of header */
	        me->EOLstate = EOL_END;
	    else if (WHITE(*b))		       /* Folding: LF SP or CR LF SP */
		me->EOLstate = EOL_FOLD;
	    else						/* New line */
		me->EOLstate = EOL_LINE;
	} else if (me->EOLstate == EOL_SCR) {
	    if (*b==CR || *b==LF)			    /* End of header */
	        me->EOLstate = EOL_END;
	    else if (WHITE(*b))		 /* Folding: LF CR SP or CR LF CR SP */
		me->EOLstate = EOL_FOLD;
	    else						/* New line */
		me->EOLstate = EOL_LINE;
	} else if (*b == CR)
	    me->EOLstate = EOL_FCR;
	else if (*b == LF)
	    me->EOLstate = EOL_FLF;			       /* Line found */
	else {
	    if (!me->haveToken) {
	        if (*b == ':' || isspace(*b)) {
		    HTChunk_putb(me->token, start, end-start);
		    HTChunk_putc(me->token, '\0');
		    me->haveToken = YES;
		} else {
		    unsigned char ch = *(unsigned char *) b;
		    tolower(ch);
/*		    if (ch >= 'A' && ch <= 'Z')
		        ch += ('a' - 'A'); */
		    me->hash = (me->hash * 3 + ch) % MIME_HASH_SIZE;
		}
	    } else if (value == NULL && *b != ':' && !isspace(*b))
	        value = b;
	    end++;
	}
	switch (me->EOLstate) {
	    case EOL_LINE:
	    case EOL_END: {
	        int status;
		HTChunk_putb(me->value, value, end-value);
		HTChunk_putc(me->value, '\0');
		start=b, end=b;
		status = HTRequest_dispatchMIMEParse(me->request, 
						     HTChunk_data(me->token), 
						     HTChunk_data(me->value));
		if (me->EOLstate == EOL_END) {		/* EOL_END */
		    if (status == HT_OK)
		        status = pumpData(me);
		    HTNet_setBytesRead(me->net, l);
	        } else {				/* EOL_LINE */
		    HTChunk_clear(me->token);
		    HTChunk_clear(me->value);
		    me->haveToken = NO;
		    me->hash = 0;
		    value = NULL;
		}
		me->EOLstate = EOL_BEGIN;
		if (status != HT_OK)
		    return status;
		break;
	        }
	    case EOL_FOLD:
		me->EOLstate = EOL_BEGIN;
	        if (!me->haveToken) {
		    HTChunk_putb(me->token, start, end-start);
		    HTChunk_putc(me->token, '\0');
		    me->haveToken = YES;
	        } else if (value) {
		    HTChunk_putb(me->value, value, end-value);
		    HTChunk_putc(me->value, ' ');
		}
		start=b, end=b;
		break;
	    default: 
	        b++;
	        l--;
	        if (!l) {
		    if (!me->haveToken)
		        HTChunk_putb(me->token, start, end-start);
		    else if (value)
		        HTChunk_putb(me->value, value, end-value);
		    return HT_OK;
		}
	}
    }

    /* 
    ** Put the rest down the stream without touching the data but make sure
    ** that we get the correct content length of data
    */
    if ((status = (*me->target->isa->put_block)(me->target, b, l)) != HT_OK)
        return status;
    /* Check if CL at all - thanks to jwei@hal.com (John Wei) */
    cl = HTAnchor_length(me->anchor);
    return (cl>=0 && HTNet_bytesRead(me->net)>=cl) ? HT_LOADED : HT_OK;
}


/*	Character handling
**	------------------
*/
PRIVATE int HTMIME_put_character (HTStream * me, char c)
{
    return HTMIME_put_block(me, &c, 1);
}


/*	String handling
**	---------------
*/
PRIVATE int HTMIME_put_string (HTStream * me, const char * s)
{
    return HTMIME_put_block(me, s, (int) strlen(s));
}


/*	Flush an stream object
**	---------------------
*/
PRIVATE int HTMIME_flush (HTStream * me)
{
    return me->target ? (*me->target->isa->flush)(me->target) : HT_OK;
}

/*	Free a stream object
**	--------------------
*/
PRIVATE int HTMIME_free (HTStream * me)
{
    int status = HT_OK;
    if (!me->transparent)
        if (HTRequest_dispatchMIMEParse(me->request, HTChunk_data(me->token), 
					HTChunk_data(me->value)) == HT_OK)
	    pumpData(me);
    if (me->target) {
	if ((status = (*me->target->isa->_free)(me->target))==HT_WOULD_BLOCK)
	    return HT_WOULD_BLOCK;
    }
    if (PROT_TRACE)
	HTTrace("MIME........ FREEING....\n");
    HTChunk_delete(me->token);
    HTChunk_delete(me->value);
    HT_FREE(me);
    return status;
}

/*	End writing
*/
PRIVATE int HTMIME_abort (HTStream * me, HTList * e)
{
    int status = HT_ERROR;
    if (me->target) status = (*me->target->isa->abort)(me->target, e);
    if (PROT_TRACE)
	HTTrace("MIME........ ABORTING...\n");
    HTChunk_delete(me->token);
    HTChunk_delete(me->value);
    HT_FREE(me);
    return status;
}



/*	Structured Object Class
**	-----------------------
*/
PRIVATE const HTStreamClass HTMIME =
{		
	"MIMEParser",
	HTMIME_flush,
	HTMIME_free,
	HTMIME_abort,
	HTMIME_put_character,
	HTMIME_put_string,
	HTMIME_put_block
}; 


/*	MIME header parser stream.
**	-------------------------
**	This stream parses a complete MIME header and if a content type header
**	is found then the stream stack is called. Any left over data is pumped
**	right through the stream
*/
PUBLIC HTStream* HTMIMEConvert (HTRequest *	request,
				void *		param,
				HTFormat	input_format,
				HTFormat	output_format,
				HTStream *	output_stream)
{
    HTStream * me;
    if ((me = (HTStream *) HT_CALLOC(1, sizeof(* me))) == NULL)
        HT_OUTOFMEM("HTMIMEConvert");
    me->isa = &HTMIME;       
    me->request = request;
    me->anchor = request->anchor;
    me->net = request->net;
    me->target = output_stream;
    me->target_format = output_format;
    me->token = HTChunk_new(256);
    me->value = HTChunk_new(256);
    me->hash = 0;
    me->EOLstate = EOL_BEGIN;
    me->haveToken = NO;
    return me;
}

/*	MIME header ONLY parser stream
**	------------------------------
**	This stream parses a complete MIME header and then returnes HT_PAUSE.
**	It does not set up any streams and resting data stays in the buffer.
**	This can be used if you only want to parse the headers before you
**	decide what to do next. This is for example the case in a server app.
*/
PUBLIC HTStream * HTMIMEHeader (HTRequest *	request,
				void *		param,
				HTFormat	input_format,
				HTFormat	output_format,
				HTStream *	output_stream)
{
    HTStream * me = HTMIMEConvert(request, param, input_format,
				  output_format, output_stream);
    me->head_only = YES;
    return me;
}

/*	MIME footer ONLY parser stream
**	------------------------------
**	Parse only a footer, for example after a chunked encoding.
*/
PUBLIC HTStream * HTMIMEFooter (HTRequest *	request,
				void *		param,
				HTFormat	input_format,
				HTFormat	output_format,
				HTStream *	output_stream)
{
    HTStream * me = HTMIMEConvert(request, param, input_format,
				  output_format, output_stream);
    me->footer = YES;
    return me;
}
