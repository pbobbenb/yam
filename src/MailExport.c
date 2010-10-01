/***************************************************************************

 YAM - Yet Another Mailer
 Copyright (C) 1995-2000 by Marcel Beck <mbeck@yam.ch>
 Copyright (C) 2000-2010 by YAM Open Source Team

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
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 YAM Official Support Site :  http://www.yam.ch
 YAM OpenSource project    :  http://sourceforge.net/projects/yamos/

 $Id$

***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clib/alib_protos.h>
#include <proto/dos.h>
#include <proto/intuition.h>

#include "extrasrc.h"

#include "YAM.h"
#include "YAM_mainFolder.h"
#include "YAM_stringsizes.h"
#include "YAM_utilities.h"

#include "FileInfo.h"
#include "Locale.h"
#include "MailList.h"
#include "MUIObjects.h"

#include "mui/Classes.h"
#include "tcp/Connection.h"

#include "Debug.h"

struct TransferContext
{
  struct Connection *conn;
  Object *transferGroup;
  char transferGroupTitle[SIZE_DEFAULT]; // the TransferControlGroup's title
  struct MinList transferList;
};

/// ExportMails
//  Saves a list of messages to a MBOX mailbox file
BOOL ExportMails(const char *fname, const struct MailList *mlist, const BOOL quiet, const BOOL append)
{
  BOOL success = FALSE;
  struct TransferContext tc;

  ENTER();

  if((tc.conn = CreateConnection()) != NULL)
  {
    snprintf(tc.transferGroupTitle, sizeof(tc.transferGroupTitle), tr(MSG_TR_MailTransferTo), fname);

    if((tc.transferGroup = (Object *)DoMethod(G->App, MUIM_YAM_CreateTransferGroup, TR_EXPORT, tc.transferGroupTitle, tc.conn, quiet == FALSE)) != NULL)
    {
      BOOL abort = FALSE;
      struct MailNode *mnode;
      int numberOfMails = 0;
      ULONG totalSize = 0;
      int i;
      struct Node *curNode;

      // reset our processing list
      NewMinList(&tc.transferList);

      // temporarly copy all data out of our mlist to the
      // processing list and mark all mails as "to be transferred"
      LockMailListShared(mlist);

      i = 0;
      ForEachMailNode(mlist, mnode)
      {
        struct Mail *mail = mnode->mail;

        if(mail != NULL)
        {
          struct MailTransferNode *mtn;

          if((mtn = AllocSysObjectTags(ASOT_NODE, ASONODE_Size, sizeof(*mtn),
                                                  ASONODE_Min, TRUE,
                                                  TAG_DONE)) != NULL)
          {
            memset(mtn, 0, sizeof(*mtn));

            if((mtn->mail = memdup(mail, sizeof(*mail))) != NULL)
            {
              mtn->index = i + 1;

              // set to TRANSFER
              mtn->tflags = TRF_TRANSFER;

              numberOfMails++;
              totalSize += mail->Size;

              AddTail((struct List *)&tc.transferList, (struct Node *)mtn);
            }
            else
            {
              // we end up in a low memory condition, lets exit
              // after having freed everything
              FreeSysObject(ASOT_NODE, mtn);
              abort = TRUE;
              break;
            }
          }
          else
          {
            // we end up in a low memory condition, lets exit
            abort = TRUE;
            break;
          }
        }

        i++;
      }

      UnlockMailList(mlist);

      // if we have now something in our processing list,
      // lets go on
      if(abort == FALSE && IsMinListEmpty(&tc.transferList) == FALSE)
      {
        FILE *fh;

        DoMethod(tc.transferGroup, MUIM_TransferControlGroup_Start, numberOfMails, totalSize);

        // open our final destination file either in append or in a fresh
        // write mode.
        if((fh = fopen(fname, append ? "a" : "w")) != NULL)
        {
          setvbuf(fh, NULL, _IOFBF, SIZE_FILEBUF);

          success = TRUE;

          IterateList(&tc.transferList, curNode)
          {
            struct MailTransferNode *mtn = (struct MailTransferNode *)curNode;
            struct Mail *mail = mtn->mail;
            char mailfile[SIZE_PATHFILE];
            char fullfile[SIZE_PATHFILE];

            // update the transfer status
            DoMethod(tc.transferGroup, MUIM_TransferControlGroup_Next, mtn->index, -1, mail->Size, tr(MSG_TR_Exporting));

            GetMailFile(mailfile, sizeof(mailfile), NULL, mail);
            if(StartUnpack(mailfile, fullfile, mail->Folder) != NULL)
            {
              FILE *mfh;

              // open the message file to start exporting it
              if((mfh = fopen(fullfile, "r")) != NULL)
              {
                char datstr[64];
                char *buf = NULL;
                size_t buflen = 0;
                ssize_t curlen;
                BOOL inHeader = TRUE;

                // printf out our leading "From " MBOX format line first
                DateStamp2String(datstr, sizeof(datstr), &mail->Date, DSS_UNIXDATE, TZC_NONE);
                fprintf(fh, "From %s %s", mail->From.Address, datstr);

                // let us put out the Status: header field
                fprintf(fh, "Status: %s\n", MA_ToStatusHeader(mail));

                // let us put out the X-Status: header field
                fprintf(fh, "X-Status: %s\n", MA_ToXStatusHeader(mail));

                // now we iterate through every line of our mail and try to substitute
                // found "From " line with quoted ones
                while(tc.conn->abort == FALSE &&
                      (curlen = getline(&buf, &buflen, mfh)) > 0)
                {
                  char *tmp = buf;

                  // check if this is a single \n so that it
                  // signals the end if a line
                  if(buf[0] == '\n' || (buf[0] == '\r' && buf[1] == '\n'))
                  {
                    inHeader = FALSE;

                    if(fwrite(buf, curlen, 1, fh) != 1)
                    {
                      // write error, bail out
                      break;
                    }

                    continue;
                  }

                  // the mboxrd format specifies that we need to quote any
                  // From, >From, >>From etc. occurance.
                  // http://www.qmail.org/man/man5/mbox.html
                  while(*tmp == '>')
                    tmp++;

                  if(strncmp(tmp, "From ", 5) == 0)
                  {
                    if(fputc('>', fh) == EOF)
                    {
                      // write error, bail out
                      break;
                    }
                  }
                  else if(inHeader == TRUE)
                  {
                    // let us skip some specific headerlines
                    // because we placed our own here
                    if(strncmp(buf, "Status: ", 8) == 0 ||
                       strncmp(buf, "X-Status: ", 10) == 0)
                    {
                      // skip line
                      continue;
                    }
                  }

                  // write the line to our destination file
                  if(fwrite(buf, curlen, 1, fh) != 1)
                  {
                    // write error, bail out
                    break;
                  }

                  // make sure we have a newline at the end of the line
                  if(buf[curlen-1] != '\n')
                  {
                    if(fputc('\n', fh) == EOF)
                    {
                      // write error, bail out
                      break;
                    }
                  }

                  // update the transfer status
                  DoMethod(tc.transferGroup, MUIM_TransferControlGroup_Update, curlen, tr(MSG_TR_Exporting));
                }

                // check why we exited the while() loop and if everything is fine
                if(tc.conn->abort == TRUE)
                {
                  D(DBF_NET, "export was aborted by the user");
                  success = FALSE;
                }
                else if(ferror(fh) != 0)
                {
                  E(DBF_NET, "error on writing data! ferror(fh)=%ld", ferror(fh));

                  // an error occurred, lets return failure
                  success = FALSE;
                }
                else if(ferror(mfh) != 0 || feof(mfh) == 0)
                {
                  E(DBF_NET, "error on reading data! ferror(mfh)=%ld feof(mfh)=%ld", ferror(mfh), feof(mfh));

                  // an error occurred, lets return failure
                  success = FALSE;
                }

                // close file pointer
                fclose(mfh);

                free(buf);

                // put the transferStat to 100%
                DoMethod(tc.transferGroup, MUIM_TransferControlGroup_Update, TCG_SETMAX, tr(MSG_TR_Exporting));
              }
              else
               success = FALSE;

              FinishUnpack(fullfile);
            }
            else
              success = FALSE;

            if(tc.conn->abort == TRUE || success == FALSE)
              break;
          }

          // close file pointer
          fclose(fh);

          // write the status to our logfile
          mnode = FirstMailNode(mlist);
          AppendToLogfile(LF_ALL, 51, tr(MSG_LOG_Exporting), numberOfMails, mnode->mail->Folder->Name, fname);
        }

        DoMethod(tc.transferGroup, MUIM_TransferControlGroup_Finish);
      }

      // delete all nodes in our temporary list
      while((curNode = RemHead((struct List *)&tc.transferList)) != NULL)
      {
        struct MailTransferNode *mtn = (struct MailTransferNode *)curNode;

        // free the mail pointer
        free(mtn->mail);

        // free the node itself
        FreeSysObject(ASOT_NODE, mtn);
      }

      DoMethod(G->App, MUIM_YAM_DeleteTransferGroup, tc.transferGroup);
    }
  }

  DeleteConnection(tc.conn);

  RETURN(success);
  return success;
}

///