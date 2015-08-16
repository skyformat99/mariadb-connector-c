/* MariaDB Communication IO (CIO) plugin for shate memory communication
 *
 * During initialization MariaDB serve creates a named file mapping
 * object, named : *
 *
 * */

#ifdef _WIN32

#include <my_global.h>
#include <my_sys.h>
#include <errmsg.h>
#include <mysql.h>
#include <mysql/client_plugin.h>
#include <string.h>
#include <m_string.h>

#ifndef HAVE_NPIPE_DEFAULT
#define my_malloc(A, B) malloc((A))
#undef my_free
#define my_free(A,B) free(((A)))
#endif



my_bool cio_shm_set_timeout(MARIADB_CIO *cio, enum enum_cio_timeout type, int timeout);
int cio_shm_get_timeout(MARIADB_CIO *cio, enum enum_cio_timeout type);
size_t cio_shm_read(MARIADB_CIO *cio, uchar *buffer, size_t length);
size_t cio_shm_write(MARIADB_CIO *cio, uchar *buffer, size_t length);
int cio_shm_wait_io_or_timeout(MARIADB_CIO *cio, my_bool is_read, int timeout);
my_bool cio_shm_blocking(MARIADB_CIO *cio, my_bool value, my_bool *old_value);
my_bool cio_shm_connect(MARIADB_CIO *cio, MA_CIO_CINFO *cinfo);
my_bool cio_shm_close(MARIADB_CIO *cio);
int cio_shm_fast_send(MARIADB_CIO *cio);
int cio_shm_keepalive(MARIADB_CIO *cio);
my_socket cio_shm_get_socket(MARIADB_CIO *cio);
my_bool cio_shm_is_blocking(MARIADB_CIO *cio);

struct st_ma_cio_methods cio_shm_methods= {
  cio_shm_set_timeout,
  cio_shm_get_timeout,
  cio_shm_read,
  cio_shm_write,
  cio_shm_wait_io_or_timeout,
  cio_shm_blocking,
  cio_shm_connect,
  cio_shm_close,
  cio_shm_fast_send,
  cio_shm_keepalive,
  cio_shm_get_socket,
  cio_shm_is_blocking
};

MARIADB_CIO_PLUGIN _mysql_client_plugin_declaration_ =
{
  MYSQL_CLIENT_CIO_PLUGIN,
  MYSQL_CLIENT_CIO_PLUGIN_INTERFACE_VERSION,
  "cio_shm",
  "Georg Richter",
  "MariaDB communication IO plugin for named pipe communication",
  {1, 0, 0},
  "LGPL",
  NULL,
  NULL,
  &cio_shm_methods,
  NULL,
  NULL
};

struct st_cio_shm {
  HANDLE pipe;
  OVERLAPPED overlapped;
  size_t rw_size;
  int fcntl_mode;
  MYSQL *mysql;
};

my_bool cio_shm_set_timeout(MARIADB_CIO *cio, enum enum_cio_timeout type, int timeout)
{
  if (!cio)
    return 1;
  cio->timeout[type]= (timeout > 0) ? timeout * 1000 : -1;
  return 0;
}

int cio_shm_get_timeout(MARIADB_CIO *cio, enum enum_cio_timeout type)
{
  if (!cio)
    return -1;
  return cio->timeout[type] / 1000;
}

size_t cio_shm_read(MARIADB_CIO *cio, uchar *buffer, size_t length)
{
  DWORD dwRead= 0;
  size_t r= -1;
  struct st_cio_shm *cpipe= NULL;

  if (!cio || !cio->data)
    return -1;

  cpipe= (struct st_cio_shm *)cio->data;

  if (ReadFile(cpipe->pipe, buffer, length, &dwRead, &cpipe->overlapped))
  {
    r= (size_t)dwRead;
    goto end;
  }
  if (GetLastError() == ERROR_IO_PENDING)
    r= cio_shm_wait_io_or_timeout(cio, 1, 0);
  
  if (!r)
    r= cpipe->rw_size;
end:  
  return r;
}

size_t cio_shm_write(MARIADB_CIO *cio, uchar *buffer, size_t length)
{
  DWORD dwWrite= 0;
  size_t r= -1;
  struct st_cio_shm *cpipe= NULL;

  if (!cio || !cio->data)
    return -1;

  cpipe= (struct st_cio_shm *)cio->data;

  if (WriteFile(cpipe->pipe, buffer, length, &dwWrite, &cpipe->overlapped))
  {
    r= (size_t)dwWrite;
    goto end;
  }
  if (GetLastError() == ERROR_IO_PENDING)
    r= cio_shm_wait_io_or_timeout(cio, 1, 0);
  
  if (!r)
    r= cpipe->rw_size;
end:  
  return r;
}

int cio_shm_wait_io_or_timeout(MARIADB_CIO *cio, my_bool is_read, int timeout)
{
  int r= -1;
  DWORD status;
  int save_error;
  struct st_cio_shm *cpipe= NULL;

  cpipe= (struct st_cio_shm *)cio->data;

  if (!timeout)
    timeout= (is_read) ? cio->timeout[CIO_READ_TIMEOUT] : cio->timeout[CIO_WRITE_TIMEOUT];

  status= WaitForSingleObject(cpipe->overlapped.hEvent, timeout);
  if (status == WAIT_OBJECT_0)
  {
    if (GetOverlappedResult(cpipe->pipe, &cpipe->overlapped, &cpipe->rw_size, FALSE))
      return 0;
  }  
  /* other status codes are: WAIT_ABANDONED, WAIT_TIMEOUT and WAIT_FAILED */
  save_error= GetLastError();
  CancelIo(cpipe->pipe);
  SetLastError(save_error);
  return -1;
}

my_bool cio_shm_blocking(MARIADB_CIO *cio, my_bool block, my_bool *previous_mode)
{
  /* not supported */
  return 0;
}

int cio_shm_keepalive(MARIADB_CIO *cio)
{
  /* not supported */
  return 0;
}

int cio_shm_fast_send(MARIADB_CIO *cio)
{
  /* not supported */
  return 0;
}
my_bool cio_shm_connect(MARIADB_CIO *cio, MA_CIO_CINFO *cinfo)
{
  struct st_cio_shm *cpipe= NULL;

  if (!cio || !cinfo)
    return 1;

  if (!(cpipe= (struct st_cio_shm *)my_malloc(sizeof(struct st_cio_shm), MYF(0))))
  {
    CIO_SET_ERROR(cinfo->mysql, CR_OUT_OF_MEMORY, unknown_sqlstate, 0, "");
    return 1;
  }
  bzero(cpipe, sizeof(struct st_cio_shm));
  cio->data= (void *)cpipe;
  cpipe->pipe= INVALID_HANDLE_VALUE;
  cio->mysql= cinfo->mysql;
  cio->type= cinfo->type;

  if (cinfo->type == CIO_TYPE_NAMEDPIPE)
  {
    my_bool has_timedout= 0;
    char szPipeName[MAX_PATH];
    DWORD dwMode;

    if ( ! cinfo->unix_socket || (cinfo->unix_socket)[0] == 0x00)
      cinfo->unix_socket = MYSQL_NAMEDPIPE;
    if (!cinfo->host || !strcmp(cinfo->host,LOCAL_HOST))
      cinfo->host=LOCAL_HOST_NAMEDPIPE;

    szPipeName[MAX_PATH - 1]= 0;
    snprintf(szPipeName, MAX_PATH - 1, "\\\\%s\\pipe\\%s", cinfo->host, cinfo->unix_socket);

    while (1)
    {
      if ((cpipe->pipe = CreateFile(szPipeName,
                                    GENERIC_READ |
                                    GENERIC_WRITE,
                                    0,               /* no sharing */
                                    NULL,            /* default security attributes */
                                    OPEN_EXISTING,
                                    0,               /* default attributes */
                                    NULL)) != INVALID_HANDLE_VALUE)
        break;

      if (GetLastError() != ERROR_PIPE_BUSY)
      {
        cio->set_error(cio, CR_NAMEDPIPEOPEN_ERROR, SQLSTATE_UNKNOWN, 0,
                       cinfo->host, cinfo->unix_socket, GetLastError());
        goto end;
      }

      if (has_timedout || !WaitNamedPipe(szPipeName, cio->timeout[CIO_CONNECT_TIMEOUT]))
      {
        cio->set_error(cio, CR_NAMEDPIPEWAIT_ERROR, SQLSTATE_UNKNOWN, 0,
                       cinfo->host, cinfo->unix_socket, GetLastError());
        goto end;
      }
      has_timedout= 1;
    }

    dwMode = PIPE_READMODE_BYTE | PIPE_WAIT;
    if (!SetNamedPipeHandleState(cpipe->pipe, &dwMode, NULL, NULL))
    {
      cio->set_error(cio, CR_NAMEDPIPESETSTATE_ERROR, SQLSTATE_UNKNOWN, 0,
                     cinfo->host, cinfo->unix_socket, (ulong) GetLastError());
      goto end;
    }

    /* Register event handler for overlapped IO */
    if (!(cpipe->overlapped.hEvent= CreateEvent(NULL, FALSE, FALSE, NULL)))
    {
      cio->set_error(cio, CR_CREATE_EVENT_FAILED, SQLSTATE_UNKNOWN, 0,
                     GetLastError());
      goto end;
    }
    return 0;
  }
end:
  if (cpipe)
  {
    if (cpipe->pipe != INVALID_HANDLE_VALUE)
      CloseHandle(cpipe->pipe);
    my_free((gptr)cpipe, MYF(0));
    cio->data= NULL;
  }
  return 1;
}

my_bool cio_shm_close(MARIADB_CIO *cio)
{
  struct st_cio_shm *cpipe= NULL;
  int r= 0;

  if (!cio)
    return 1;

  if (cio->data)
  {
    cpipe= (struct st_cio_shm *)cio->data;
    CloseHandle(cpipe->overlapped.hEvent);
    if (cpipe->pipe != INVALID_HANDLE_VALUE)
    {
      CloseHandle(cpipe->pipe);
      cpipe->pipe= INVALID_HANDLE_VALUE;
    }
    my_free((gptr)cio->data, MYF(0));
    cio->data= NULL;
  }
  return r;
}

my_socket cio_shm_get_socket(MARIADB_CIO *cio)
{
  if (cio && cio->data)
    return (my_socket)((struct st_cio_shm *)cio->data)->pipe;
  return INVALID_SOCKET;
} 

my_bool cio_shm_is_blocking(MARIADB_CIO *cio)
{
  return 1;
}

#endif