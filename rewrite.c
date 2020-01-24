#define FUSE_USE_VERSION 31
#define _GNU_SOURCE

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fuse.h>
#include <fuse_opt.h>
#include <pcre.h>

#include "rewrite.h"

#define DEBUG(lvl, x...) if(config.verbose >= lvl) fprintf(stderr, x)

/*
 * Type definiton 
 */
struct regexp {
    pcre *regexp;
    pcre_extra *extra;
    int captures;
    int replace_all;
    char *raw;
};

struct replacement_part {
    int group;
    char *data;
    int len;
};

struct replacement_template {
    int nparts;
    char *raw;
    struct replacement_part *parts;
};

struct rewrite_rule {
    struct regexp *filename_regexp;
    struct replacement_template *rewritten_path; /* NULL for "." */
    struct rewrite_rule *next;
};

struct rewrite_context {
    struct regexp *cmdline; /* NULL for all contexts */
    struct rewrite_rule *rules;
    struct rewrite_context *next;
};

struct config {
    char *config_file;
    char *orig_fs;
    int orig_fd;
    char *mount_point;
    struct rewrite_context *contexts;
    int verbose;
    int autocreate;
};

enum type {
    CMDLINE,
    RULE,
    END
};

/*
 * Global variables
 */
static struct config config;

/*
 * Config-file parsing
 */
static void *abmalloc(size_t sz) {
    void *res = malloc(sz);
    if(res == NULL) {
        perror("malloc");
        abort();
    }
    return res;
}

/* Consume all blanks (according to isspace) */
static void parse_blanks(FILE *fd) {
    int c;
    do {
        c = getc(fd);
    } while(isspace(c) && c != EOF);
    ungetc(c, fd);
}

/* Consume all characters until reaching EOL */
static void parse_comment(FILE *fd) {
    int c;
    do {
        c = getc(fd);
    } while(c != '\n' && c != EOF);
}

static char *string_new(int *string_cap, int *string_size) {
    *string_cap = 255;
    *string_size = 0;
    char *s = abmalloc(*string_cap);
    s[0] = '\0';
    return s;
}

/* append c to string, extending it if necessary */
static void string_append(char **string, char c, int *string_cap, int *string_size) {
    if(*string_cap == *string_size + 1) {
        *string_cap *= 2;
        *string = realloc(*string, *string_cap);
        if(*string == NULL) {
            perror("realloc");
            abort();
        }
    }
    
    (*string)[(*string_size)++] = c;
    (*string)[*string_size] = 0;
}

/* Consume the string until reaching sep */
static void parse_string(FILE *fd, char **string, char sep) {
    int string_cap, string_size;
    int escaped = 0;
    int c;
    
    *string = string_new(&string_cap, &string_size);
    for(;;) {
        c = getc(fd);
        if(c == EOF) {
            fprintf(stderr, "Unexpected EOF\n");
            exit(1);
        }

        if(escaped) {
            /* \\ -> \
             * \(sep) -> (sep)
             * \(other) -> \(other)
             */
            escaped = 0;
            if(c != '\\' && c != (int)sep) {
                string_append(string, '\\', &string_cap, &string_size);
            }
            string_append(string, c, &string_cap, &string_size);
        } else {
            if(c == '\\') {
                escaped = 1;
            } else if(c == (int)sep) {
                break;
            } else {
                string_append(string, c, &string_cap, &string_size);
            }
        }
    }
}

/* Consume the regexp (until reaching end-of-flags) and put it in regexp */
static void parse_regexp(FILE *fd, struct regexp **regexp, char sep) {
    char *regexp_body;
    int regexp_flags = 0;
    int replace_all = 0;
    const char *error;
    int offset;
    int c;
    
    /* Determine separator */
    if(sep == 0) {
        sep = getc(fd);
        if(sep == 'm') {
            sep = getc(fd);
        } else if(sep != '/') {
            fprintf(stderr, "Unexpected character \"%c\"\n", (char)sep);
            exit(1);
        }
    }
    
    if(sep == EOF) {
        fprintf(stderr, "Unexpected EOF\n");
        exit(1);
    }
    
    /* Get body */
    parse_string(fd, &regexp_body, sep);
    
    /* Get flags */
    while(!isspace(c = getc(fd))) {
        switch(c) {
        case 'i':
            regexp_flags |= PCRE_CASELESS;
            break;
        case 'x':
            regexp_flags |= PCRE_EXTENDED;
            break;
        case 'u':
            regexp_flags |= PCRE_UCP | PCRE_UTF8;
            break;
        case 'g':
            replace_all = 1;
            break;
        case EOF:
            fprintf(stderr, "Unexpected EOF\n");
            exit(1);
        default:
            fprintf(stderr, "Unknown flag %c\n", (char)c);
            exit(1);
        }
    }
    
    /* Compilation */
    *regexp = abmalloc(sizeof(struct regexp));

    (*regexp)->replace_all = replace_all;
    
    (*regexp)->regexp = pcre_compile(regexp_body, regexp_flags, &error, &offset, NULL);
    if((*regexp)->regexp == NULL) {
        fprintf(stderr, "Invalid regular expression: %s\n. Regular expression was :\n  %s\n", error, regexp_body);
        exit(1);
    }
    
    (*regexp)->extra = pcre_study((*regexp)->regexp, 0, &error);
    if((*regexp)->extra == NULL && error != NULL) {
        fprintf(stderr, "Can't compile regular expression: %s\n. Regular expression was :\n  %s\n", error, regexp_body);
        exit(1);
    }
    
    pcre_fullinfo((*regexp)->regexp, (*regexp)->extra, PCRE_INFO_CAPTURECOUNT, &(*regexp)->captures);
    (*regexp)->raw = regexp_body;
}

/* Get a CMDLINE or RULE definition */
static void parse_item(FILE *fd, enum type *type, struct regexp **regexp, char **string) {
    int c;
    
    parse_blanks(fd);
    switch(c = getc(fd)) {
    case '-':
        *type = CMDLINE;
        parse_blanks(fd);
        parse_regexp(fd, regexp, 0);
        return;
    case 'm':
        c = getc(fd);
        /* continue */
    case '/':
        *type = RULE;
        parse_regexp(fd, regexp, (char)c);
        parse_blanks(fd);
        parse_string(fd, string, '\n');
        return;
    case '#':
        parse_comment(fd);
        parse_item(fd, type, regexp, string);
        return;
    case EOF:
        *type = END;
        return;
    default:
        fprintf(stderr, "Unexpected character \"%c\"\n", (char)c);
        exit(1);
    }
}

static struct replacement_part *alloc_part(struct replacement_part **parts, int *nparts) {
    *parts = reallocarray(*parts, ++(*nparts), sizeof(struct replacement_part));
    if(*parts == NULL) {
        perror("reallocarray");
        abort();
    }
    return (*parts) + (*nparts - 1);
}

static struct replacement_template *parse_replacement_template(char *tpl) {
    int buf_cap, buf_size;
    char *buf = string_new(&buf_cap, &buf_size);

    int nparts = 0;
    struct replacement_part *parts = NULL;
    struct replacement_part *cur_part;

    int escaped = 0;
    for(int i = 0; i < strlen(tpl); i++) {
        if(escaped) {
            if(tpl[i] >= '0' && tpl[i] <= '9') {
                if(buf_size > 0) {
                    cur_part = alloc_part(&parts, &nparts);
                    cur_part->data = buf;
                    cur_part->len = buf_size;
                    buf = string_new(&buf_cap, &buf_size);
                }

                cur_part = alloc_part(&parts, &nparts);
                cur_part->data = NULL;
                cur_part->group = tpl[i] - '0';
            } else {
                string_append(&buf, tpl[i], &buf_cap, &buf_size);
            }
            escaped = 0;
        } else {
            if(tpl[i] == '\\') {
                escaped = 1;
            } else {
                string_append(&buf, tpl[i], &buf_cap, &buf_size);
            }
        }
    }

    if(nparts == 0 || buf_size > 0) {
        cur_part = alloc_part(&parts, &nparts);
        cur_part->data = buf;
        cur_part->len = buf_size;
    }

    struct replacement_template *res = abmalloc(sizeof(struct replacement_template));
    res->parts = parts;
    res->nparts = nparts;
    res->raw = tpl;

    return res;
}

static void parse_config(FILE *fd) {
    enum type type;
    struct regexp *regexp;
    char *string;
    
    struct rewrite_rule *rule, *last_rule = NULL;
    
    struct rewrite_context *new_context;
    struct rewrite_context *current_context = abmalloc(sizeof(struct rewrite_context));
    current_context->cmdline = NULL;
    current_context->rules = NULL;
    current_context->next = NULL;
    config.contexts = current_context;
    
    do {
        parse_item(fd, &type, &regexp, &string);
        if(type == CMDLINE) {
            new_context = abmalloc(sizeof(struct rewrite_context));
            new_context->cmdline = !strcmp(regexp->raw, "") ? NULL : regexp;
            new_context->rules = last_rule = NULL;
            new_context->next = NULL;
            current_context->next = new_context;
            current_context = new_context;
        } else if(type == RULE) {
            rule = abmalloc(sizeof(struct rewrite_rule));
            rule->filename_regexp = regexp;
            rule->rewritten_path = (!strcmp(string, ".")) ? (free(string), NULL) : parse_replacement_template(string);
            rule->next = NULL;
            if(last_rule)
                last_rule->next = rule;
            last_rule = rule;
            if(current_context->rules == NULL)
                current_context->rules = rule;
        }
    } while(type != END);
}

/*
 * Command-line arguments parsing
 */
enum {
    KEY_HELP,
    KEY_VERSION,
};

#define REWRITE_OPT(t, p, v) { t, offsetof(struct config, p), v }

static struct fuse_opt options[] = {
    REWRITE_OPT("config=%s",       config_file, 0),
    REWRITE_OPT("verbose=%i",      verbose, 0),
    REWRITE_OPT("autocreate",      autocreate, 1),

    FUSE_OPT_KEY("-V",             KEY_VERSION),
    FUSE_OPT_KEY("--version",      KEY_VERSION),
    FUSE_OPT_KEY("-h",             KEY_HELP),
    FUSE_OPT_KEY("--help",         KEY_HELP),
    FUSE_OPT_END
};

static int options_proc(void *data, const char *arg, int key, struct fuse_args *outargs) {
    switch(key) {
    case FUSE_OPT_KEY_NONOPT:
        if(config.orig_fs == NULL) {
            config.orig_fs = strdup(arg);
            return 0;
        } else if(config.mount_point == NULL) {
            config.mount_point = strdup(arg);
            return 1;
        } else {
            fprintf(stderr, "Invalid argument: %s\n", arg);
            exit(1);
        }
        break;

    case KEY_HELP:
        fprintf(stderr,
                "usage: %s [-o options] source mountpoint\n"
                "\n"
                "rewritefs options:\n"
                "    -o opt,[opt...]  mount options (see mount.fuse)\n"
                "    -h   --help      Fuse help\n"
                "    -V   --version   print version\n"
                "    -f               foreground\n"
                "    -d               debug\n"
                "    -o config=CONFIG path to configuration file\n"
                "    -o verbose=LEVEL verbose level [to be used with -f or -d] (LEVEL is 1 to 4)\n"
                "\n",
                outargs->argv[0]);
        fuse_opt_add_arg(outargs, "-ho");
        fuse_main(outargs->argc, outargs->argv, NULL, NULL);
        exit(0);

    case KEY_VERSION:
        fuse_opt_add_arg(outargs, "--version");
        fuse_main(outargs->argc, outargs->argv, NULL, NULL);
        exit(0);
    }
    return 1;
}

void parse_args(int argc, char **argv, struct fuse_args *outargs) {
    FILE *fd;
    
    memset(&config, 0, sizeof(config));
    fuse_opt_parse(outargs, &config, options, options_proc);
    fuse_opt_add_arg(outargs, "-o");
    fuse_opt_add_arg(outargs, "default_permissions");

    if(config.orig_fs == NULL) {
        fprintf(stderr, "missing source argument\n");
        exit(1);
    } else {
        config.orig_fd = open(config.orig_fs, O_PATH);
        if(config.orig_fd == -1) {
            fprintf(stderr, "Cannot open source directory: %s\n", strerror(errno));
            exit(1);
        }
    }

    if(config.mount_point == NULL) {
        fprintf(stderr, "missing mount point argument\n");
        exit(1);
    }
   
    if(config.config_file) {
        fd = fopen(config.config_file, "r");
        if(fd == NULL) {
            perror("opening config file");
            exit(1);
        }
        parse_config(fd);
        fclose(fd);
        
        struct rewrite_context *ctx;
        struct rewrite_rule *rule;
        for(ctx = config.contexts; ctx != NULL; ctx = ctx->next) {
            DEBUG(1, "CTX \"%s\":\n", ctx->cmdline ? ctx->cmdline->raw : "default");
            for(rule = ctx->rules; rule != NULL; rule = rule->next)
                DEBUG(1, "  \"%s\" -> \"%s\"\n", rule->filename_regexp->raw, rule->rewritten_path ? rule->rewritten_path->raw : "(don't rewrite)");
        }
        DEBUG(1, "\n");
    }
}

/*
 * Rewrite stuff
 */
char *get_caller_cmdline() {
    char path[PATH_MAX];
    FILE *fd;
    int size = 0, cap = 255, c;
    char *ret = malloc(cap);
    
    if(ret == NULL) {
        return NULL;
    } else {
        *ret = 0;
    }
    
    snprintf(path, PATH_MAX, "/proc/%d/cmdline", fuse_get_context()->pid);
    fd = fopen(path, "r");
    if(fd == NULL)
        return ret;
    
    while((c = getc(fd)) != EOF) {
        if(c == 0)
            c = ' ';
        string_append(&ret, c, &cap, &size);
    }
    
    fclose(fd);
    
    return ret;
}

/* Recursively create all parent directories in `path`. */
static int mkdir_parents(const char *path, mode_t mode) {
    int result = 0;

    /* dirname() could clobber its argument. */
    char *path_ = strdup(path);

    /* dirname() may statically allocate the result; we need to copy so we don’t */
    char *dir = strdup(dirname(path_));

    struct stat dirstat;
    errno = 0;
    if ((fstatat(config.orig_fd, dir, &dirstat, 0) == -1) && errno == ENOENT) {
        if (mkdir_parents(dir, mode)) {
            result = -1;
            goto done;
        }

        WLOCK(result = mkdirat(config.orig_fd, dir, mode));
        if (result == -1)
            goto done;
    }

done:
    free(dir);
    free(path_);
    return result;
}

char *regexp_replace(struct regexp *re, const char *subject, struct replacement_template *tpl) {
    int nvec, repl_sz, *ovector = NULL;
    char *result, *repl, *repl_buf = NULL, *suffix_buf = NULL;
    const char *suffix;
    
    /* Fill ovector */
    nvec = (re->captures + 1) * 3;
    ovector = calloc(nvec, sizeof(int));
    if(ovector == NULL) {
        result = NULL;
        goto end;
    }

    int scount = pcre_exec(re->regexp, re->extra, subject,
                           strlen(subject), 0, 0, ovector, nvec);

    if(scount == PCRE_ERROR_NOMATCH)
        return strdup(subject);

    /* Replace backreferences */
    if(tpl->nparts > 1 || tpl->parts[0].data == NULL) {
        int i, wpos, group;

        for(i = 0, repl_sz = 0; i < tpl->nparts; i++) {
            if(tpl->parts[i].data == NULL) {
                group = tpl->parts[i].group;
                if(group < scount) {
                    repl_sz += ovector[group*2+1] - ovector[group*2];
                }
            } else {
                repl_sz += tpl->parts[i].len;
            }
        }

        repl = repl_buf = malloc(repl_sz + 1);
        if(repl == NULL) {
            result = NULL;
            goto end;
        }

        for(i = 0, wpos = 0; i < tpl->nparts; i++) {
            if(tpl->parts[i].data == NULL) {
                group = tpl->parts[i].group;
                if(group < scount) {
                    strncpy(repl + wpos, subject + ovector[group*2], ovector[group*2+1]-ovector[group*2]);
                    wpos += ovector[group*2+1] - ovector[group*2];
                }
            } else {
                strcpy(repl + wpos, tpl->parts[i].data);
                wpos += tpl->parts[i].len;
            }
        }
        repl[wpos] = '\0';
    } else {
        repl = tpl->parts[0].data;
        repl_sz = tpl->parts[0].len;
    }

    suffix = subject + ovector[1];
    if(re->replace_all && suffix[0] != '\0') {
        suffix = suffix_buf = regexp_replace(re, suffix, tpl);
        if(suffix == NULL) {
            result = NULL;
            goto end;
        }
    }

    DEBUG(4, "  subject = %s\n", subject);
    DEBUG(4, "  prefix = %s\n", strndup(subject, ovector[0]));
    DEBUG(4, "  replaced match = %s\n", repl);
    DEBUG(4, "  suffix = %s\n", suffix);

    result = malloc(ovector[0] + repl_sz + strlen(suffix) + 1);
    if(result == NULL) {
        result = NULL;
        goto end;
    }

    result[0] = 0;
    strncat(result, subject, ovector[0]);
    strcat(result, repl);
    strcat(result, suffix);

end:
    free(repl_buf);
    free(suffix_buf);
    free(ovector);

    return result;
}

char *apply_rule(const char *path, struct rewrite_rule *rule) {
    if(rule == NULL || rule->rewritten_path == NULL) {
        DEBUG(2, "  (ignored) %s -> %s\n", path, path + 1);
        DEBUG(3, "\n");
        return strdup(path[1] == '\0' ? "." : path+1);
    }

    char *rewritten = regexp_replace(rule->filename_regexp, path + 1, rule->rewritten_path);

    if(config.autocreate) {
        if(mkdir_parents(rewritten, (S_IRWXU | S_IRWXG | S_IRWXO)) == -1)
            fprintf(stderr, "Warning: %s -> %s: autocreating parents failed: %s\n",
                    path, rewritten, strerror(errno));
    }

    DEBUG(1, "  %s -> %s\n", path, rewritten);
    DEBUG(3, "\n");

    return rewritten;
}

char *rewrite(const char *path) {
    struct rewrite_context *ctx;
    struct rewrite_rule *rule;
    char *caller = NULL;
    
    int res;
    
    DEBUG(3, "%s:\n", path);
    
    for(ctx = config.contexts; ctx != NULL; ctx = ctx->next) {
        if(ctx->cmdline) {
            if(!caller) {
                caller = get_caller_cmdline();
                if(caller == NULL) {
                    fprintf(stderr, "WARNING: cannot obtain caller command line\n");
                    continue;
                }
            }
            res = pcre_exec(ctx->cmdline->regexp, ctx->cmdline->extra, caller,
                strlen(caller), 0, 0, NULL, 0);
            if(res < 0) {
                if(res != PCRE_ERROR_NOMATCH)
                    fprintf(stderr, "WARNING: pcre_exec returned %d\n", res);
                DEBUG(3, "  CTX NOMATCH \"%s\"\n", ctx->cmdline->raw);
                continue;
            }
            DEBUG(3, "  CTX OK \"%s\"\n", ctx->cmdline->raw);
        } else {
            DEBUG(3, "  CTX DEFAULT\n");
        }
        
        for(rule = ctx->rules; rule != NULL; rule = rule->next) {
            res = pcre_exec(rule->filename_regexp->regexp, rule->filename_regexp->extra, path + 1,
                strlen(path) - 1, 0, 0, NULL, 0);
            if(res < 0) {
                if(res != PCRE_ERROR_NOMATCH)
                    fprintf(stderr, "WARNING: pcre_exec returned %d\n", res);
                DEBUG(3, "    RULE NOMATCH \"%s\"\n", rule->filename_regexp->raw);
            } else {
                DEBUG(3, "    RULE OK \"%s\" \"%s\"\n", rule->filename_regexp->raw, rule->rewritten_path ? rule->rewritten_path->raw : "(don't rewrite)");
                free(caller);
                return apply_rule(path, rule);
            }
        }
    }
    
    free(caller);
    return apply_rule(path, NULL);
}

int orig_fd() {
    return config.orig_fd;
}
