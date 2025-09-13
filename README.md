This is the first part in my "trilogy" of building my own OS. It simply imitates real bash behavior: redirection parser, a few built-in commands (from which I only implemented cd and exit), and all the external commands are linux's executable files (usually stored in /usr/bin) which I run by execvp.

The redirection parser only supports: < input redirection, >,>> output redirection, 2>,2>> error redirection.
  
Apart from that, all the external commands work the same as they do in real bash - because they just invoke the actual command files.

**Compile+Run** - in WSL:

> gcc *.c -o basic-bash
> 
> ./basic-bash
