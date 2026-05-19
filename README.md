markdown# EduOS — Operating Systems Simulator

Module Code: 351 CS 2104
Module Name: Operating Systems
Student Name: Maziko Khonje
Registration Number: 25-311-351-014
Semester: III

## Project Overview

EduOS is a multi-component operating system simulator built in C and Python.
It demonstrates core OS concepts including process management, thread pools,
inter-process communication, and CPU scheduling algorithms. The C core handles
low-level simulation while Python handles scheduling visualisation.

## Prerequisites

### For the C components:
- GCC compiler (MinGW on Windows)
- POSIX threads support (-pthread)
- Valgrind (Linux/WSL recommended for memory checks)

### For the Python components:
- Python 3.8 or higher
- pip (Python package manager)

## Build Instructions

### C Core
cd c_core
make all
### Python Scheduler
cd python_scheduler
pip install -r requirements.txt
python scheduler_sim.py --random 10
### Run Full Integration
cd controller
python main_controller.py
## Directory Structure
EduOS-25311351014/
├── README.md               # Project documentation
├── .gitignore              # Excludes build artifacts and cache files
├── docs/
│   ├── report.pdf          # Final written report
│   └── screenshots/        # All Gantt charts and comparison charts
├── c_core/
│   ├── Makefile            # Build targets: all, clean, race, fixed, memcheck
│   ├── include/
│   │   └── eduos.h         # Shared struct definitions (PCB, etc.)
│   ├── process_manager.c   # edu_fork, edu_exec, edu_wait, edu_exit, edu_ps
│   ├── thread_manager.c    # Thread pool, race condition demo, deadlock fix
│   ├── ipc_module.c        # Shared memory and anonymous pipe IPC
│   └── main_sim.c          # Driver that runs all C components
├── python_scheduler/
│   ├── scheduler_sim.py    # All 4 scheduling algorithms
│   ├── gantt.py            # Gantt chart and visualisation code
│   ├── sample_processes.csv # Sample input file
│   └── requirements.txt    # Python dependencies
└── controller/
└── main_controller.py  # Integration bridge between C and Python

## Screenshots

*(To be added once the simulator is running)*

## Valgrind Output

*(To be pasted here once memcheck is complete)*

## Challenges and Solutions

*(To be filled in as development progresses)*

## References

- Silberschatz, A., Galvin, P. B., & Gagne, G. - Operating System Concepts, 10th Edition
- Linux man pages: fork(2), execve(2), wait(2), pthread_create(3)
- Course notes - 351 CS 2104 Operating Systems
