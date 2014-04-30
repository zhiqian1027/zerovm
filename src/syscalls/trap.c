/*
 * Copyright (c) 2012, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <sys/mman.h>
#include "src/channels/channel.h"
#include "src/main/report.h"
#include "src/main/setup.h"
#include "src/syscalls/snapshot.h"
#include "src/syscalls/daemon.h"
#include "src/syscalls/ztrace.h"
#include "src/loader/userspace.h"
#include "src/loader/usermap.h"

/*
 * read specified amount of bytes from given desc/offset to buffer
 * return amount of read bytes or negative error code if call failed
 */
static int32_t ZVMReadHandle(struct NaClApp *nap,
    int ch, char *buffer, int32_t size, int64_t offset)
{
  struct ChannelDesc *channel;
  int64_t tail;
  char *sys_buffer;

  assert(nap != NULL);
  assert(nap->manifest != NULL);
  assert(nap->manifest->channels != NULL);

  /* check the channel number */
  if(ch < 0 || ch >= nap->manifest->channels->len)
  {
    ZLOGS(LOG_DEBUG, "channel_id=%d, buffer=%p, size=%d, offset=%ld",
        ch, buffer, size, offset);
    return -EINVAL;
  }
  channel = CH_CH(nap->manifest, ch);
  ZLOGS(LOG_INSANE, "channel %s, buffer=%p, size=%d, offset=%ld",
      channel->alias, buffer, size, offset);

  /* check other arguments sanity */
  if(size < 0) return -EFAULT;
  if(offset < 0) return -EINVAL;
  if(size == 0) return 0;

  /* check buffer availability */
  sys_buffer = (char*)NaClUserToSysAddrNullOkay((uintptr_t)buffer);
  if(CheckUserMap((uintptr_t)sys_buffer, size, PROT_WRITE) == -1)
    return -EINVAL;

  /* ignore user offset for sequential access read */
  if(CH_SEQ_READABLE(channel))
    offset = channel->getpos;
  /* prevent reading beyond the end of the random access channels */
  else
  {
    size = MIN(channel->size - offset, size);
    if(size == 0) return 0;
  }

  /* check for eof */
  if(channel->eof) return 0;

  /* check limits */
  if(channel->counters[GetsLimit] >= channel->limits[GetsLimit])
    return -EDQUOT;

  /* calculate i/o leftovers */
  tail = channel->limits[GetSizeLimit] - channel->counters[GetSizeLimit];
  if(size > tail) size = tail;
  if(size < 1) return -EDQUOT;

  /* read data */
  return ChannelRead(channel, sys_buffer, (size_t)size, (off_t)offset);
}

/*
 * write specified amount of bytes from buffer to given desc/offset
 * return amount of read bytes or negative error code if call failed
 */
static int32_t ZVMWriteHandle(struct NaClApp *nap,
    int ch, const char *buffer, int32_t size, int64_t offset)
{
  struct ChannelDesc *channel;
  int64_t tail;
  const char *sys_buffer;

  assert(nap != NULL);
  assert(nap->manifest != NULL);
  assert(nap->manifest->channels != NULL);

  /* check the channel number */
  if(ch < 0 || ch >= nap->manifest->channels->len)
  {
    ZLOGS(LOG_DEBUG, "channel_id=%d, buffer=%p, size=%d, offset=%ld",
        ch, buffer, size, offset);
    return -EINVAL;
  }
  channel = CH_CH(nap->manifest, ch);
  ZLOGS(LOG_INSANE, "channel %s, buffer=%p, size=%d, offset=%ld",
      channel->alias, buffer, size, offset);

  /* check other arguments sanity */
  if(size < 0) return -EFAULT;
  if(offset < 0) return -EINVAL;
  if(size == 0) return 0;

  /* check buffer availability */
  sys_buffer = (char*)NaClUserToSysAddrNullOkay((uintptr_t)buffer);
  if(CheckUserMap((uintptr_t)sys_buffer, size, PROT_READ) == -1)
    return -EINVAL;

  /* ignore user offset for sequential access write */
  if(CH_SEQ_WRITEABLE(channel)) offset = channel->putpos;

  /* check limits */
  if(channel->counters[PutsLimit] >= channel->limits[PutsLimit])
    return -EDQUOT;
  tail = channel->limits[PutSizeLimit] - channel->counters[PutSizeLimit];

  /* prevent writing beyond the limit */
  if(CH_RND_WRITEABLE(channel))
    if(offset >= channel->limits[PutSizeLimit])
      return -EINVAL;

  if(offset >= channel->size + tail) return -EINVAL;
  if(size > tail) size = tail;
  if(size < 1) return -EDQUOT;

  /* write data */
  return ChannelWrite(channel, sys_buffer, (size_t)size, (off_t)offset);
}

/*
 * put protection on memory region addr:size. available protections are
 * r/o, r/w, r/x, none. if user asked for r/x validation will be applied
 * returns 0 (successful) or negative error code
 */
static int32_t ZVMProtHandle(uintptr_t addr, uint32_t size, int prot)
{
  int result = 0;
  uintptr_t sysaddr;

  sysaddr = NaClUserToSysAddrNullOkay(addr);

  /* sanity check */
  if(size % NACL_MAP_PAGESIZE != 0)
    return -EINVAL;
  if(sysaddr % NACL_MAP_PAGESIZE != 0)
    return -EINVAL;

  /* locked regions are not allowed to change protection */
  if(CheckUserMap(sysaddr, size, 0) < 0)
    return -EACCES;

  /* put protection */
  switch(prot)
  {
    case PROT_NONE:
    case PROT_READ:
    case PROT_WRITE:
    case PROT_READ | PROT_WRITE:
      if(Zmprotect((void*)sysaddr, size, prot) != 0)
        result = -errno;
      break;

    case PROT_EXEC:
    case PROT_READ | PROT_EXEC:
      /* test if memory is readable */
      if(CheckUserMap(sysaddr, size, PROT_READ) != 0)
      {
        result = -EACCES;
        break;
      }

      /* validation failed */
      if(NaClSegmentValidates((uint8_t*)sysaddr, size, addr) == 0)
        result = -EPERM;
      /* validation ok, changing protection */
      else
        if(Zmprotect((void*)sysaddr, size, prot) != 0)
          result = -errno;
      break;

    default:
      result = -EPERM;
      break;
  }

  return result;
}

/* user exit. session is finished. no return. */
static void ZVMExitHandle(struct NaClApp *nap, uint64_t code)
{
  assert(nap != NULL);

  ReportSetupPtr()->user_code = code;
  if(ReportSetupPtr()->zvm_code == 0)
    ReportSetupPtr()->zvm_state = g_strdup(OK_STATE);
  ZLOGS(LOG_DEBUG, "SESSION %s RETURNED %lu", nap->manifest->node, code);
  SessionDtor(0, OK_STATE);
}

/* handler for syscalls testing */
static void ZVMTestHandle(struct NaClApp *nap)
{
  assert(nap != NULL);
  SaveSession(nap);
}

int32_t TrapHandler(struct NaClApp *nap, uint32_t args)
{
  uint64_t *sargs;
  int retcode = 0;

  assert(nap != NULL);
  assert(nap->manifest != NULL);

  /* test args address validity */
  if(args > -48u) return -EFAULT;

  /*
   * translate address from user space to system
   * note: cannot set "trap error"
   */
  sargs = (uint64_t*)NaClUserToSys((uintptr_t)args);
  ZLOGS(LOG_DEBUG, "%s called", FunctionName(*sargs));
  ZTrace("untrusted code");

  switch(*sargs)
  {
    case TrapFork:
      retcode = Daemon(nap);
      if(retcode) break;
      SyscallZTrace(*sargs, 0);
      SyscallZTrace(TrapExit, 0);
      ZVMExitHandle(nap, 0);
      break;
    case TrapExit:
      SyscallZTrace(*sargs, sargs[2]);
      ZVMExitHandle(nap, sargs[2]);
      break;
    case TrapRead:
      retcode = ZVMReadHandle(nap,
          (int)sargs[2], (char*)sargs[3], (int32_t)sargs[4], sargs[5]);
      break;
    case TrapWrite:
      retcode = ZVMWriteHandle(nap,
          (int)sargs[2], (char*)sargs[3], (int32_t)sargs[4], sargs[5]);
      break;
    case TrapProt:
      retcode = ZVMProtHandle((uint32_t)sargs[2], (uint32_t)sargs[3],
          (int)sargs[4]);
      break;
    case TrapTest:
      ZVMTestHandle(nap);
      ZVMExitHandle(nap, 0);
      break;
    default:
      retcode = -EPERM;
      ZLOG(LOG_ERROR, "function %ld is not supported", *sargs);
      break;
  }

  /* ztrace and return */
  ZLOGS(LOG_DEBUG, "%s returned %d", FunctionName(*sargs), retcode);
  SyscallZTrace(*sargs, retcode, sargs[2], sargs[3], sargs[4], sargs[5]);
  return retcode;
}
