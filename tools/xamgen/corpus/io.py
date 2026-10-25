"""IoCreateFile corpus: a plain open, and opens of a path's target directory,
which open the directory holding the last component and say whether that
exists. Everything is under game:\\krnlgen, which the first scenario makes.

The target directory's handle is checked through its attributes. A failed
open's handle is captured instead of closed.
"""

import struct

DOS_DEVICES = 0xFFFFFFFD
OBJ_CASE_INSENSITIVE = 0x40
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
SYNCHRONIZE = 0x00100000
FILE_ATTRIBUTE_DIRECTORY = 0x10
FILE_ATTRIBUTE_NORMAL = 0x80
FILE_SHARE_READ_WRITE = 3
FILE_OPEN = 1
FILE_CREATE = 2
FILE_OPEN_IF = 3
FILE_OVERWRITE_IF = 5
FILE_DIRECTORY_FILE = 0x1
FILE_SYNCHRONOUS_IO_NONALERT = 0x20
FILE_NON_DIRECTORY_FILE = 0x40
OPEN_TARGET_DIRECTORY = 0x4
FILE_NETWORK_OPEN_INFORMATION = 34
NETWORK_OPEN_INFORMATION_SIZE = 0x38

ROOT = 'game:\\krnlgen'
FILE = ROOT + '\\file'


def attributes(s, path):
    """OBJECT_ATTRIBUTES naming `path` under \\??."""
    text = s.string(path, 'path')
    name = s.alloc(8, 'name', struct.pack('>HHI', len(path), len(path) + 1,
                                          text.address))
    return s.alloc(0xC, 'attributes', struct.pack(
        '>III', DOS_DEVICES, name.address, OBJ_CASE_INSENSITIVE))


def create(s, path, access, file_attributes, disposition, create_options,
           options=None, capture_status=True, capture_handle=False):
    """NtCreateFile, or IoCreateFile with `options`. Returns the handle's
    Variable."""
    handle_out = s.alloc(4, 'handle')
    iosb = s.alloc(8, 'iosb')
    args = [handle_out, access, attributes(s, path), iosb, 0, file_attributes,
            FILE_SHARE_READ_WRITE, disposition, create_options]
    export = 'NtCreateFile'
    if options is not None:
        export = 'IoCreateFile'
        args.append(options)
    handle = s.variable('handle')
    capture = ([iosb] if capture_status else []) + (
        [handle_out] if capture_handle else [])
    s.call(export, *args, capture=capture, store=[(handle, handle_out)])
    return handle


def query_attributes(s, handle):
    iosb = s.alloc(8, 'query iosb')
    # Zeroed, so the padding after the attributes reads alike written or not.
    info = s.alloc(NETWORK_OPEN_INFORMATION_SIZE, 'network open information',
                   fill=0)
    s.call('NtQueryInformationFile', handle, iosb, info,
           NETWORK_OPEN_INFORMATION_SIZE, FILE_NETWORK_OPEN_INFORMATION,
           capture=[iosb, info + 0x30])


def target_directory(s, path):
    handle = create(s, path, GENERIC_READ | SYNCHRONIZE, 0, FILE_OPEN,
                    FILE_SYNCHRONOUS_IO_NONALERT, OPEN_TARGET_DIRECTORY)
    query_attributes(s, handle)
    s.call('NtClose', handle)


def build(corpus):
    # Whether these exist from an earlier run shows only in what's not
    # captured.
    s = corpus.scenario('tree')
    handle = create(s, ROOT, GENERIC_READ | SYNCHRONIZE,
                    FILE_ATTRIBUTE_DIRECTORY, FILE_OPEN_IF,
                    FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False)
    s.call('NtClose', handle)
    handle = create(s, FILE, GENERIC_WRITE | SYNCHRONIZE,
                    FILE_ATTRIBUTE_NORMAL, FILE_OVERWRITE_IF,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False)
    s.call('NtClose', handle)

    s = corpus.scenario('open file')
    handle = create(s, FILE, GENERIC_READ | SYNCHRONIZE, 0, FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, 0)
    s.call('NtClose', handle)

    for name, path in (('existing file', FILE),
                       ('missing file', ROOT + '\\missing'),
                       ('directory', ROOT)):
        target_directory(corpus.scenario(f'target of {name}'), path)

    s = corpus.scenario('target in missing directory')
    create(s, ROOT + '\\missing\\file', GENERIC_READ | SYNCHRONIZE, 0,
           FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, OPEN_TARGET_DIRECTORY,
           capture_handle=True)

    for name, path, disposition, create_options in (
            ('open missing file', ROOT + '\\missing', FILE_OPEN,
             FILE_NON_DIRECTORY_FILE),
            ('create existing file', FILE, FILE_CREATE,
             FILE_NON_DIRECTORY_FILE),
            ('open directory as a file', ROOT, FILE_OPEN,
             FILE_NON_DIRECTORY_FILE)):
        create(corpus.scenario(name), path, GENERIC_READ | SYNCHRONIZE, 0,
               disposition, create_options | FILE_SYNCHRONOUS_IO_NONALERT,
               capture_handle=True)

    # The size is checked before anything is read, so nothing is mounted.
    s = corpus.scenario('StfsCreateDevice wrong size')
    params = s.alloc(0x58, 'parameters', fill=0)
    s.call('StfsCreateDevice', params, 0x50)
