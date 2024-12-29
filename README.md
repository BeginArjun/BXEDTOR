### BXEDTOR - A TERMINAL BASED TEXT EDITOR

**BXEDTOR** is a lightweight text editor written in C, designed for basic text manipulation. It is minimalistic and compiled using the GCC compiler.  

---

## Features

- Insert rows into the editor dynamically.
- Manage memory effectively for rows.
- Simple and easy-to-read code.

---

## Prerequisites

To build and run **BXEDTOR**, you need:

- **Linux OS**: The editor is designed to run on Linux.  
  (It is untested on macOS and will not work on Windows due to dependencies on Linux-specific features.)
- **GCC Compiler**: Ensure GCC is installed on your system.

---

## Setup and Build

1. Clone or download the project files to your local machine.  
2. Open a terminal in the project directory.  
3. Compile the code using the provided Makefile:  

   ```bash
   make
   ```

This will generate an executable named `BXEDTOR`.

---

## Usage

Run the editor from the terminal using the following command:

```bash
./BXEDTOR
```

---

## File Structure

- `main.c`: Contains the implementation of the editor functionality.  
- `Makefile`: Automates the build process for the editor.  

---

## Notes

- The code uses modern C standards (`-std=c99`) for compatibility and safety.  
- Warnings and extra checks are enabled during compilation (`-Wall -Wextra -pedantic`).  
- The editor is intended for Linux systems and has not been tested on other platforms.  

---

Enjoy using **BXEDTOR**! 🎉