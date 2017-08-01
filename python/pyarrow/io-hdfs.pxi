# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# ----------------------------------------------------------------------
# HDFS IO implementation

_HDFS_PATH_RE = re.compile('hdfs://(.*):(\d+)(.*)')

try:
    # Python 3
    from queue import Queue, Empty as QueueEmpty, Full as QueueFull
except ImportError:
    from Queue import Queue, Empty as QueueEmpty, Full as QueueFull


def have_libhdfs():
    try:
        check_status(HaveLibHdfs())
        return True
    except:
        return False


def have_libhdfs3():
    try:
        check_status(HaveLibHdfs3())
        return True
    except:
        return False


def strip_hdfs_abspath(path):
    m = _HDFS_PATH_RE.match(path)
    if m:
        return m.group(3)
    else:
        return path


cdef class HadoopFileSystem:
    cdef:
        shared_ptr[CHadoopFileSystem] client

    cdef readonly:
        bint is_open

    def __cinit__(self):
        pass

    def _connect(self, host, port, user, kerb_ticket, driver):
        cdef HdfsConnectionConfig conf

        if host is not None:
            conf.host = tobytes(host)
        conf.port = port
        if user is not None:
            conf.user = tobytes(user)
        if kerb_ticket is not None:
            conf.kerb_ticket = tobytes(kerb_ticket)

        if driver == 'libhdfs':
            check_status(HaveLibHdfs())
            conf.driver = HdfsDriver_LIBHDFS
        else:
            check_status(HaveLibHdfs3())
            conf.driver = HdfsDriver_LIBHDFS3

        with nogil:
            check_status(CHadoopFileSystem.Connect(&conf, &self.client))
        self.is_open = True

    @classmethod
    def connect(cls, *args, **kwargs):
        return cls(*args, **kwargs)

    def __dealloc__(self):
        if self.is_open:
            self.close()

    def close(self):
        """
        Disconnect from the HDFS cluster
        """
        self._ensure_client()
        with nogil:
            check_status(self.client.get().Disconnect())
        self.is_open = False

    cdef _ensure_client(self):
        if self.client.get() == NULL:
            raise IOError('HDFS client improperly initialized')
        elif not self.is_open:
            raise IOError('HDFS client is closed')

    def exists(self, path):
        """
        Returns True if the path is known to the cluster, False if it does not
        (or there is an RPC error)
        """
        self._ensure_client()

        cdef c_string c_path = tobytes(path)
        cdef c_bool result
        with nogil:
            result = self.client.get().Exists(c_path)
        return result

    def isdir(self, path):
        cdef HdfsPathInfo info
        self._path_info(path, &info)
        return info.kind == ObjectType_DIRECTORY

    def isfile(self, path):
        cdef HdfsPathInfo info
        self._path_info(path, &info)
        return info.kind == ObjectType_FILE

    def info(self, path):
        """
        Return detailed HDFS information for path

        Parameters
        ----------
        path : string
            Path to file or directory

        Returns
        -------
        path_info : dict
        """
        cdef HdfsPathInfo info
        self._path_info(path, &info)
        return {
            'path': frombytes(info.name),
            'owner': frombytes(info.owner),
            'group': frombytes(info.group),
            'size': info.size,
            'block_size': info.block_size,
            'last_modified': info.last_modified_time,
            'last_accessed': info.last_access_time,
            'replication': info.replication,
            'permissions': info.permissions,
            'kind': ('directory' if info.kind == ObjectType_DIRECTORY
                     else 'file')
        }

    cdef _path_info(self, path, HdfsPathInfo* info):
        cdef c_string c_path = tobytes(path)

        with nogil:
            check_status(self.client.get()
                         .GetPathInfo(c_path, info))


    def ls(self, path, bint full_info):
        cdef:
            c_string c_path = tobytes(path)
            vector[HdfsPathInfo] listing
            list results = []
            int i

        self._ensure_client()

        with nogil:
            check_status(self.client.get()
                         .ListDirectory(c_path, &listing))

        cdef const HdfsPathInfo* info
        for i in range(<int> listing.size()):
            info = &listing[i]

            # Try to trim off the hdfs://HOST:PORT piece
            name = strip_hdfs_abspath(frombytes(info.name))

            if full_info:
                kind = ('file' if info.kind == ObjectType_FILE
                        else 'directory')

                results.append({
                    'kind': kind,
                    'name': name,
                    'owner': frombytes(info.owner),
                    'group': frombytes(info.group),
                    'list_modified_time': info.last_modified_time,
                    'list_access_time': info.last_access_time,
                    'size': info.size,
                    'replication': info.replication,
                    'block_size': info.block_size,
                    'permissions': info.permissions
                })
            else:
                results.append(name)

        return results

    def chmod(self, path, mode):
        """
        Change file permissions

        Parameters
        ----------
        path : string
            absolute path to file or directory
        mode : int
            POSIX-like bitmask
        """
        self._ensure_client()
        cdef c_string c_path = tobytes(path)
        cdef int c_mode = mode
        with nogil:
            check_status(self.client.get()
                         .Chmod(c_path, c_mode))

    def chown(self, path, owner=None, group=None):
        """
        Change file permissions

        Parameters
        ----------
        path : string
            absolute path to file or directory
        owner : string, default None
            New owner, None for no change
        group : string, default None
            New group, None for no change
        """
        cdef:
            c_string c_path
            c_string c_owner
            c_string c_group
            const char* c_owner_ptr = NULL
            const char* c_group_ptr = NULL

        self._ensure_client()

        c_path = tobytes(path)
        if owner is not None:
            c_owner = tobytes(owner)
            c_owner_ptr = c_owner.c_str()

        if group is not None:
            c_group = tobytes(group)
            c_group_ptr = c_group.c_str()

        with nogil:
            check_status(self.client.get()
                         .Chown(c_path, c_owner_ptr, c_group_ptr))

    def mkdir(self, path):
        """
        Create indicated directory and any necessary parent directories
        """
        self._ensure_client()
        cdef c_string c_path = tobytes(path)
        with nogil:
            check_status(self.client.get()
                         .MakeDirectory(c_path))

    def delete(self, path, bint recursive=False):
        """
        Delete the indicated file or directory

        Parameters
        ----------
        path : string
        recursive : boolean, default False
            If True, also delete child paths for directories
        """
        self._ensure_client()

        cdef c_string c_path = tobytes(path)
        with nogil:
            check_status(self.client.get()
                         .Delete(c_path, recursive == 1))

    def open(self, path, mode='rb', buffer_size=None, replication=None,
             default_block_size=None):
        """
        Parameters
        ----------
        mode : string, 'rb', 'wb', 'ab'
        """
        self._ensure_client()

        cdef HdfsFile out = HdfsFile()

        if mode not in ('rb', 'wb', 'ab'):
            raise Exception("Mode must be 'rb' (read), "
                            "'wb' (write, new file), or 'ab' (append)")

        cdef c_string c_path = tobytes(path)
        cdef c_bool append = False

        # 0 in libhdfs means "use the default"
        cdef int32_t c_buffer_size = buffer_size or 0
        cdef int16_t c_replication = replication or 0
        cdef int64_t c_default_block_size = default_block_size or 0

        cdef shared_ptr[HdfsOutputStream] wr_handle
        cdef shared_ptr[HdfsReadableFile] rd_handle

        if mode in ('wb', 'ab'):
            if mode == 'ab':
                append = True

            with nogil:
                check_status(
                    self.client.get()
                    .OpenWriteable(c_path, append, c_buffer_size,
                                   c_replication, c_default_block_size,
                                   &wr_handle))

            out.wr_file = <shared_ptr[OutputStream]> wr_handle

            out.is_readable = False
            out.is_writeable = 1
        else:
            with nogil:
                check_status(self.client.get()
                             .OpenReadable(c_path, &rd_handle))

            out.rd_file = <shared_ptr[RandomAccessFile]> rd_handle
            out.is_readable = True
            out.is_writeable = 0

        if c_buffer_size == 0:
            c_buffer_size = 2 ** 16

        out.mode = mode
        out.buffer_size = c_buffer_size
        out.parent = _HdfsFileNanny(self, out)
        out.is_open = True
        out.own_file = True

        return out

    def download(self, path, stream, buffer_size=None):
        with self.open(path, 'rb') as f:
            f.download(stream, buffer_size=buffer_size)

    def upload(self, path, stream, buffer_size=None):
        """
        Upload file-like object to HDFS path
        """
        with self.open(path, 'wb') as f:
            f.upload(stream, buffer_size=buffer_size)


# ARROW-404: Helper class to ensure that files are closed before the
# client. During deallocation of the extension class, the attributes are
# decref'd which can cause the client to get closed first if the file has the
# last remaining reference
cdef class _HdfsFileNanny:
    cdef:
        object client
        object file_handle_ref

    def __cinit__(self, client, file_handle):
        import weakref
        self.client = client
        self.file_handle_ref = weakref.ref(file_handle)

    def __dealloc__(self):
        fh = self.file_handle_ref()
        if fh:
            fh.close()
        # avoid cyclic GC
        self.file_handle_ref = None
        self.client = None


cdef class HdfsFile(NativeFile):
    cdef readonly:
        int32_t buffer_size
        object mode
        object parent

    cdef object __weakref__

    def __dealloc__(self):
        self.parent = None
