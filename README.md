# mdpp

Markdown preprocessor.

## What?

This project is an extension around the idea of wanting to be able to run shell
commands at "compile time" in my
[markdown](https://daringfireball.net/projects/markdown/) documents.

The syntax is inspired by the
[command substitution](https://www.gnu.org/software/bash/manual/html_node/Command-Substitution.html)
syntax found in POSIX-esque shell implementations (since <code>\`...\`</code>
is already found in markdown for code we use the `$(...)` syntax).

Additionally I wanted to be able to define a document "header" containing
information to be include in the HTML `<head>`, and also defining functions /
variables for use in substitions.

Eventually the goal is to allow custom directive definitions so you could
define simple transformations (although these would be limited to the realm of
direct substitutions / shell commands). An example use case of this would be
defining a `$$...$$` directive which translates into something that renders
LaTex expressions.

## Goals/TODO

- [x] Command substitution
- [x] Simple compilation (no autotools)
- [ ] Contextual awarness of Markdown (no substitutions in code blocks)
- [ ] Shell persistance across substitutions
- [ ] Header section
- [ ] Simple I/O testing
- [ ] Auto invoke markdown command
- [ ] Custom directives
- [ ] Custom shells

## References

1. [Daring Fireball: Markdown](https://daringfireball.net/projects/markdown/)
1. [Preprocessor - Wikipedia](https://en.wikipedia.org/wiki/Preprocessor)
1. [dylan-lom/shmd on GitHub](https://github.com/dylan-lom/shmd)

