/*-------------------------------------------------------------------------
 *
 * parsexlog.c
 *	  Functions for reading Write-Ahead-Log
 *
 * Portions Copyright (c) 2013-2015 VMware, Inc. All Rights Reserved.
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#define FRONTEND 1
#include "c.h"
#undef FRONTEND
#include "postgres.h"

#include "pg_rewind.h"
#include "filemap.h"

#include <unistd.h>

#include "access/rmgr.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "catalog/pg_control.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"


/*
 * RmgrNames is an array of resource manager names, to make error messages
 * a bit nicer.
 */
#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup) \
  name,

static const char *RmgrNames[RM_MAX_ID + 1] = {
#include "access/rmgrlist.h"
};

static void extractPageInfo(XLogReaderState *record);

static int xlogreadfd = -1;
static XLogSegNo xlogreadsegno = -1;
static char xlogfpath[MAXPGPATH];

typedef struct XLogPageReadPrivate
{
	const char	   *datadir;
	TimeLineID		tli;
} XLogPageReadPrivate;

static int SimpleXLogPageRead(XLogReaderState *xlogreader,
				   XLogRecPtr targetPagePtr,
				   int reqLen, XLogRecPtr targetRecPtr, char *readBuf,
				   TimeLineID *pageTLI);

/*
 * Read all the WAL in the datadir/pg_xlog,  starting from 'startpoint' on
 * timeline 'tli'. Make note of the data blocks touched by the WAL records,
 * and return them in a page map.
 */
void
extractPageMap(const char *datadir, XLogRecPtr startpoint, TimeLineID tli)
{
	XLogRecord *record;
	XLogReaderState *xlogreader;
	char *errormsg;
	XLogPageReadPrivate private;

	private.datadir = datadir;
	private.tli = tli;
	xlogreader = XLogReaderAllocate(&SimpleXLogPageRead, &private);

	record = XLogReadRecord(xlogreader, startpoint, &errormsg);
	if (record == NULL)
	{
		fprintf(stderr, "could not read WAL starting at %X/%X",
				(uint32) (startpoint >> 32),
				(uint32) (startpoint));
		if (errormsg)
			fprintf(stderr, ": %s", errormsg);
		fprintf(stderr, "\n");
		exit(1);
	}

	do
	{
		extractPageInfo(xlogreader);

		record = XLogReadRecord(xlogreader, InvalidXLogRecPtr, &errormsg);

		if (errormsg)
			fprintf(stderr, "error reading xlog record: %s\n", errormsg);
	} while(record != NULL);

	XLogReaderFree(xlogreader);
	if (xlogreadfd != -1)
	{
		close(xlogreadfd);
		xlogreadfd = -1;
	}
}

/*
 * Reads one WAL record. Returns the end position of the record, without
 * doing anything the record itself.
 */
XLogRecPtr
readOneRecord(const char *datadir, XLogRecPtr ptr, TimeLineID tli)
{
	XLogRecord *record;
	XLogReaderState *xlogreader;
	char	   *errormsg;
	XLogPageReadPrivate private;
	XLogRecPtr	endptr;

	private.datadir = datadir;
	private.tli = tli;
	xlogreader = XLogReaderAllocate(&SimpleXLogPageRead, &private);

	record = XLogReadRecord(xlogreader, ptr, &errormsg);
	if (record == NULL)
	{
		fprintf(stderr, "could not read WAL record at %X/%X",
				(uint32) (ptr >> 32), (uint32) (ptr));
		if (errormsg)
			fprintf(stderr, ": %s", errormsg);
		fprintf(stderr, "\n");
		exit(1);
	}
	endptr = xlogreader->EndRecPtr;

	XLogReaderFree(xlogreader);
	if (xlogreadfd != -1)
	{
		close(xlogreadfd);
		xlogreadfd = -1;
	}

	return endptr;
}

/*
 * Find the previous checkpoint preceding given WAL position.
 */
void
findLastCheckpoint(const char *datadir, XLogRecPtr forkptr, TimeLineID tli,
				   XLogRecPtr *lastchkptrec, TimeLineID *lastchkpttli,
				   XLogRecPtr *lastchkptredo)
{
	/* Walk backwards, starting from the given record */
	XLogRecord *record;
	XLogRecPtr searchptr;
	XLogReaderState *xlogreader;
	char	   *errormsg;
	XLogPageReadPrivate private;


	/*
	 * The given fork pointer points to the end of the last common record,
	 * which is not necessarily the beginning of the next record, if the
	 * previous record happens to end at a page boundary. Skip over the
	 * page header in that case to find the next record.
	 */
	if (forkptr % XLOG_BLCKSZ == 0)
		forkptr += (forkptr % XLogSegSize == 0) ? SizeOfXLogLongPHD : SizeOfXLogShortPHD;

	private.datadir = datadir;
	private.tli = tli;
	xlogreader = XLogReaderAllocate(&SimpleXLogPageRead, &private);

	searchptr = forkptr;
	for (;;)
	{
		uint8		info;

		record = XLogReadRecord(xlogreader, searchptr, &errormsg);

		if (record == NULL)
		{
			fprintf(stderr, "could not find previous WAL record at %X/%X",
					(uint32) (searchptr >> 32),
					(uint32) (searchptr));
			if (errormsg)
				fprintf(stderr, ": %s", errormsg);
			fprintf(stderr, "\n");
			exit(1);
		}

		/*
		 * Check if it is a checkpoint record. This checkpoint record
		 * needs to be the latest checkpoint before WAL forked and not
		 * the checkpoint where the master has been stopped to be
		 * rewinded.
		 */
		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;
		if (searchptr < forkptr &&
			XLogRecGetRmid(xlogreader) == RM_XLOG_ID &&
			(info == XLOG_CHECKPOINT_SHUTDOWN || info == XLOG_CHECKPOINT_ONLINE))
		{
			CheckPoint	checkPoint;

			memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
			*lastchkptrec = searchptr;
			*lastchkpttli = checkPoint.ThisTimeLineID;
			*lastchkptredo = checkPoint.redo;
			break;
		}

		/* Walk backwards to previous record. */
		searchptr = record->xl_prev;
	}

	XLogReaderFree(xlogreader);
	if (xlogreadfd != -1)
	{
		close(xlogreadfd);
		xlogreadfd = -1;
	}
}

/* XLogreader callback function, to read a WAL page */
int
SimpleXLogPageRead(XLogReaderState *xlogreader, XLogRecPtr targetPagePtr,
				   int reqLen, XLogRecPtr targetRecPtr, char *readBuf,
				   TimeLineID *pageTLI)
{
	XLogPageReadPrivate *private = (XLogPageReadPrivate *) xlogreader->private_data;
	uint32		targetPageOff;
	XLogSegNo	targetSegNo PG_USED_FOR_ASSERTS_ONLY;

	XLByteToSeg(targetPagePtr, targetSegNo);
	targetPageOff = targetPagePtr % XLogSegSize;

	/*
	 * See if we need to switch to a new segment because the requested record
	 * is not in the currently open one.
	 */
	if (xlogreadfd >= 0 && !XLByteInSeg(targetPagePtr, xlogreadsegno))
	{
		close(xlogreadfd);
		xlogreadfd = -1;
	}

	XLByteToSeg(targetPagePtr, xlogreadsegno);

	if (xlogreadfd < 0)
	{
		char		xlogfname[MAXFNAMELEN];

		XLogFileName(xlogfname, private->tli, xlogreadsegno);

		snprintf(xlogfpath, MAXPGPATH, "%s/" XLOGDIR "/%s", private->datadir, xlogfname);

		xlogreadfd = open(xlogfpath, O_RDONLY | PG_BINARY, 0);

		if (xlogreadfd < 0)
		{
			fprintf(stderr, "could not open file \"%s\": %s\n", xlogfpath,
					strerror(errno));
			return -1;
		}
	}

	/*
	 * At this point, we have the right segment open.
	 */
	Assert(xlogreadfd != -1);

	/* Read the requested page */
	if (lseek(xlogreadfd, (off_t) targetPageOff, SEEK_SET) < 0)
	{
		fprintf(stderr, "could not seek in file \"%s\": %s\n", xlogfpath,
				strerror(errno));
		return -1;
	}

	if (read(xlogreadfd, readBuf, XLOG_BLCKSZ) != XLOG_BLCKSZ)
	{
		fprintf(stderr, "could not read from file \"%s\": %s\n", xlogfpath,
				strerror(errno));
		return -1;
	}

	Assert(targetSegNo == xlogreadsegno);

	*pageTLI = private->tli;
	return XLOG_BLCKSZ;
}

static void
extractPageInfo(XLogReaderState *record)
{
#define pageinfo_set_truncation(forkno, rnode, blkno) datapagemap_set_truncation(pagemap, forkno, rnode, blkno)

	int			block_id;
	RmgrId		rmid = XLogRecGetRmid(record);
	uint8		info = XLogRecGetInfo(record);
	uint8		rminfo = info & ~XLR_INFO_MASK;

	/* Is this a special record type that I recognize? */

	if (rmid == RM_DBASE_ID && rminfo == XLOG_DBASE_CREATE)
	{
		/*
		 * New databases can be safely ignored. It won't be present in the
		 * remote system, so it will be copied in toto. There's one
		 * corner-case, though: if a new, different, database is also created
		 * in the remote system, we'll see that the files already exist and
		 * not copy them. That's OK, though; WAL replay of creating the new
		 * database, from the remote WAL, will re-copy the new database,
		 * overwriting the database created in the local system.
		 */
	}
	else if (rmid == RM_DBASE_ID && rminfo == XLOG_DBASE_DROP)
	{
		/*
		 * An existing database was dropped. We'll see that the files don't
		 * exist in local system, and copy them in toto from the remote
		 * system. No need to do anything special here.
		 */
	}
	else if (rmid == RM_SMGR_ID && rminfo == XLOG_SMGR_CREATE)
	{
		/*
		 * We can safely ignore these. The local file will be removed, if it
		 * doesn't exist in remote system. If a file with same name is created
		 * in remote system, too, there will be WAL records for all the blocks
		 * in it.
		 */
	}
	else if (rmid == RM_SMGR_ID && rminfo == XLOG_SMGR_TRUNCATE)
	{
		/*
		 * We can safely ignore these. If a file is truncated locally, we'll
		 * notice that when we compare the sizes, and will copy the missing
		 * tail from remote system.
		 *
		 * TODO: But it would be nice to do some sanity cross-checking here..
		 */
	}
	else if (info & XLR_SPECIAL_REL_UPDATE)
	{
		/*
		 * This record type modifies a relation file in some special
		 * way, but we don't recognize the type. That's bad - we don't
		 * know how to track that change.
		 */
		fprintf(stderr, "WAL record modifies a relation, but record type is not recognized\n");
		fprintf(stderr, "lsn: %X/%X, rmgr: %s, info: %02X\n",
			(uint32) (record->ReadRecPtr >> 32), (uint32) (record->ReadRecPtr),
				RmgrNames[rmid], info);
		exit(1);
	}

	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		RelFileNode rnode;
		ForkNumber forknum;
		BlockNumber blkno;

		if (!XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blkno))
			continue;

		/* We only care about the main fork; others are copied in toto */
		if (forknum != MAIN_FORKNUM)
			continue;

		process_block_change(forknum, rnode, blkno);
	}
}
