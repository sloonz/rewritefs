rewritefs(1) -- mod_rewrite-like FUSE filesystem
================================================

## Description

**rewritefs** is a FUSE filesystem similar to web servers mod_rewrite. It can
change the name of accessed files on-the-fly.

The short story: I needed a tool to manage my dotfiles. I forked Luc Dufrene's
libetc [http://ordiluc.net/fs/libetc/] after it seemed unmaintained. Here is its
description:

>On my system I had way too much dotfiles:
>
>% ls -d ~/.* | wc -l
>421
>
>For easier maintenance I wrote libetc. It is a LD\_PRELOAD-able shared library
>that intercepts file operations: if a program tries to open a dotfile in
>$HOME, it is redirected to $XDG\_CONFIG\_HOME (as defined by freedesktop:
>http://standards.freedesktop.org/basedir-spec/basedir-spec-0.6.html).
>
>You can then store all your config files in $XDG\_CONFIG\_HOME instead of using
>zillions dotfiles in $HOME. If $XDG\_CONFIG\_HOME is not defined the dotfiles
>are stored in $HOME/.config/

Unfortunately, I eventually run into LD\_PRELOAD problems (mainly with programs
using dlopen like VirtualBox and screen). So I decided to rewrite it using
FUSE, and make it more generic.

## Invocation

You write your configuration file (see below), and then you mount the "rewritten"
filesystem, for example :

    rewritefs -o config=/mnt/home/me/.config/rewritefs /mnt/home/me /home/me
 
Then, accessing to files in /home/me will follow rules defined in your config
file.

## Configuration

### Regular expressions

The Regexp syntax is similar to Perl. Recognized flags are : **i**, **x**, **u**.
Example of valid regexps are:

    /foo/i
    m/fOo/u
    m/dev\/null/
    m|tata|
    m|This\sis
        \san\sextended
        \sregexep|x

Note that m{foo} is not recognized ; you must use m{foo{

**i** and **x** has the same meaning than in Perl. **u** means "use utf-8" (both for
pattern and input string).

### Command line match
 
Syntax: **-** _REGEXP_
  
Limit the following rules to programs matching REGEXP (comparing with the
content of /proc/(pid)/cmdline, replacing null characters with spaces)
  
### Rewrite rule
 
Syntax: _REGEXP_ _rewritten-path_
  
A file matching REGEXP will be rewritten to rewritten-path. To be more
accurate, the matched data will be replaced by rewritten-path in the 
filename. For example, with this rule:

    /fo/ ba

accessing to foo will be translated into bao. Warning, if you don't
start your regexp with `^`, "information" will be rewritten into
"inbamation" !
  
If rewritten-path is **.**, it means "don't rewrite anything".
  
. and .. will never be proposed to be translated.
  
You can access to matched parenthesis with \1, \2... (not yet implemented)

A regular expression can be written in more than one line, in particular in
conjunction with the **x** flag.
 
### Comment
  
A line starting with "#"

## Installation

    make && sudo make install

## Dependencies

fuse & pcre. That's all.

To use contexts, you need /proc/(pid)/cmdline. But don't use contexts if you
can avoid it !

## Performances
 
Some rules to keep the overhead smallest possible :

- use the fast pruning technique described in config.example
- avoid using contexts whenever you can
- avoid using backreferences in your regexp (\1)
- avoid using backreferences in your rewritten path. You can generally avoid
  them by using lookarounds.
  
For example, instead of writing:

    /\.(gtk-bookmarks|mysql_history)/ .cache/\1

you can write the more efficient:

    /\.(?=gtk-bookmarks|mysql_history)/ .cache/

I urge you to read "Mastering regular expressions" if you want to make
rules substantially different from the example.

## Using rewritefs with mount(8) or fstab(5)

    mount.fuse rewritefs#/mnt/home/me /home/me -o config=/mnt/home/me/.config/rewritefs,allow_other
 
allow\_other and default\_permissions is here to allow standards users to access
the filesystem with standard permissions.
 
So, you can use the fstab entry:
 
    rewritefs#/mnt/home/me /home/me fuse config=/mnt/home/me/.config/rewritefs,allow_other 0 0
 
See rewritefs --help for all FUSE options.

## Using rewritefs with pam_mount(8)

Let's suppose that you want to use rewritefs to replace libetc (it's its
primary goal, after all). You need to mount the rewritefs on your home when 
you login. This can be achieved with pam_mount.
 
Let's say you have your raw home dirs in /mnt/home/$USER. Then, to use
rewritefs on /home/$USER with configuration file stored at
/mnt/home/$USER/.config/rewritefs, you need to add this to pam_mount.xml:
 
    <volume fstype="fuse" path="rewritefs##/mnt/home/%(USER)" mountpoint="~"
         options="config=/mnt/home/%(USER)/.config/rewritefs,allow_other" />

You can add user="me" to limit this to yourself (but think to create symlinks
for other users !)
 
Don't forget to activate pam_mount in your pam configuration too. This is
distribution-dependent ; you have to refer to the corresponding documentation.

## Examples

### Example 1

This simple example show how to achieve the same effect than libetc:

    m#^(?!\.)# .
    m#^\.(cache|config|local)# .
    m#^\.# .config/


### Example 2

This example show how to use contexts ; it is like the former, but ignore the rewrite
rules for busybox:

    m#^(?!\.)# .
    m#^\.(cache|config|local)# .
    - /^\S*busybox/
    /^/ .
    - //
    m#^\.# .config/

## FAQ

Q: I installed rewritefs with the default config, and now `ls` returns me something like that :

    ls: cannot access /home/user/.vimrc: No such file or directory
    ls: cannot access /home/user/.zshrc: No such file or directory
    d????????? ? ? ? ? ? .ssh/

A: If `x` is translated (by the rules you give to rewritefs) into `y` but, and that you didn’t
renamed `x` into `y` yourself (i.e. that `x` still exists and `y` doesn’t exists), that’s the
intended behavior.

You just have to move `.vimrc` to `.config/vimrc`, `.ssh` to `.config/ssh`, and so on.

Rewritefs does not rewrite `readdir()` (it’s not possible to find `y` from `x`, since the rules
are defined using regular expressions), so `x` is listed. Then `ls` tries `stat(x)`, which is
translated into `stat(y)`, which gives this error since `y` does not exists.
