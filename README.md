# task-maker [![Build Status](https://travis-ci.org/algorithm-ninja/task-maker.svg?branch=master)](https://travis-ci.org/algorithm-ninja/task-maker)

The new cmsMake!

[![asciicast](https://asciinema.org/a/Kh8y6c7TD2WBjxTXDPnQQxPS1.png)](https://asciinema.org/a/Kh8y6c7TD2WBjxTXDPnQQxPS1)

## Installation
You have to have some dependencies installed in order to use task-maker.
This is a python3 project, you need to have Python>=3.5 as default python
environment (running `python --version`), the easiest way to achieve this is
via virtualenv.

## Usage

### Simple local usage
After installing task-maker, run `task-maker` in the task folder to compile
and run everything. Specifying no option all the caches are active, the next
executions will be very fast, actually doing only what's needed.

### Disable cache
If you really want to repeat the execution of something provide the `--cache`
option:
```bash
task-maker --cache=nothing
```

Possible values of `--cache` are: `all`, `reevaluate` (do not regenerate
inputs/outputs if not needed), `nothing` (disable all the cache).

### Test only a subset of solutions
Sometimes you only want to test only some solutions, speeding up the
compilation and cleaning a bit the output:
```bash
task-maker sol1.cpp sol2.py
```
Note that you may or may not specify the folder of the solution (sol/ or
solution/). You can also specify only the prefix of the name of the solutions
you want to check.

### Using different task directory
By default the task in the current directory is executed, if you want to change
the task without `cd`-ing away:
```bash
task-maker --task-dir ~/tasks/poldo
```

### Extracting executable files
All the compiled files are kept in an internal folder but if you want to
use them, for example to debug a solution, passing `--copy-exe` all the
useful files are copied to the `bin/` folder inside the task directory.
```bash
task-maker --copy-exe
```

### Clean the task directory
If you want to clean everything, for example after the contest, simply run:
```bash
task-maker --clean
```

### Using a remote executor
One of the best feature of task-maker is the ability to execute a task remotely.
The setup is really simple, you need to start some programs: a server and
a group of workers. The server will accept connection from workers and clients,
a worker is the program that executes a command, a client is you!

First start a server:
```bash
task-maker --run-server
```

Then start a worker in each machine, specifying the server to connect to:
```bash
task-maker --run-worker --worker-args="--server server_ip:7070"
```

To run the execution remotely just pass:
```bash
task-maker --server server_ip:7070
```

Note that the TCP port 7070 is used, the connection has to be available,
stable and reliable. A local network is suggested but it should work also
via the Internet.

### Something went wrong
If something went wrong and you want to kill task-maker you have also to kill
the background processes that may have been spawned.
You may find the pid of those processes looking at `/tmp/task-maker-*-*.pid`


## Compilation
If you want to compile tak-maker yourself you need the following dependencies:
```
g++ make cmake
```

You also need a compiler capable of compiling C++14.

To start the compilation simply run:
```
mkdir -p build
cd build
cmake ..
make
```

This will pull all the dependencies and compile everything. If you want to
speedup the compilation you may want to add `-j X` (with X the number of cores)
to `make`.

If you want to enable the optimization remember to put
`-DCMAKE_BUILD_TYPE=Release` to the `cmake` command.

Note that the build system uses Hunter for managing the dependencies, in some
cases you may find a new directory in your home called `.hunter`, to prevent
this you can add `-DHUNTER_ROOT=/some/path` to the cmake command.

## Arch Linux
If you are using Arch Linux you may want to install task-maker from the AUR:
[task-maker](https://aur.archlinux.org/packages/task-maker)
or
[task-maker-git](https://aur.archlinux.org/packages/task-maker-git).

### Manual build
We support build without Hunter (ie using system packages) only on Arch Linux,
there are some dependencies to be installed:

- `core/elfutils`
- `extra/cmake`
- `community/gmock`
- `community/gtest`
- `community/capnproto`
- `aur/pybind11`
- `community/python-ruamel-yaml`
- `community/python-traits`
- `community/python-pytoml`

To compile the project issue:
```bash
mkdir -p build
cd build
cmake .. -DHUNTER_ENABLED=OFF
make
```
