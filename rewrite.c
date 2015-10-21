#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <ctype.h>
#include <string.h>

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
    char *raw;
};

struct rewrite_rule {
    struct regexp *filename_regexp;
    char *rewritten_path; /* NULL for "." */
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
    struct rewrite_context *contexts;
    int verbose;
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
    int string_cap = 255;
    int string_size = 0;
    int escaped = 0;
    int c;
    
    *string = (char*)malloc(string_cap);
    **string = 0;
    if(*string == NULL) {
        perror("malloc");
        abort();
    }
    for(;;) {
        c = getc(fd);
        if(c == EOF) {
            fprintf(stderr, "Unexpected EOF\n");
            exit(1);
        }
        
        if(c == '\\') {
            escaped ^= 1;
        } else {
            escaped = 0;
        }
        
        if(c == (int)sep) {
            if(escaped) /* remove previous \ from the string and add the character to it */
                string_size--;
            else
                break;
        }
        
        string_append(string, c, &string_cap, &string_size);
    }
}

/* Consume the regexp (until reaching end-of-flags) and put it in regexp */
static void parse_regexp(FILE *fd, struct regexp **regexp, char sep) {
    char *regexp_body;
    int regexp_flags = 0;
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
        case EOF:
            fprintf(stderr, "Unexpected EOF\n");
            exit(1);
        default:
            fprintf(stderr, "Unknown flag %c\n", (char)c);
            exit(1);
        }
    }
    
    /* Compilation */
    *regexp = malloc(sizeof(struct regexp));
    if(regexp == NULL) {
        perror("malloc");
        abort();
    }
    
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

static void parse_config(FILE *fd) {
    enum type type;
    struct regexp *regexp;
    char *string;
    
    struct rewrite_rule *rule, *last_rule = NULL;
    
    struct rewrite_context *new_context;
    struct rewrite_context *current_context = malloc(sizeof(struct rewrite_context));
    if(current_context == NULL) {
        perror("malloc");
        abort();
    } else {
        current_context->cmdline = NULL;
        current_context->rules = NULL;
        current_context->next = NULL;
        config.contexts = current_context;
    }
    
    do {
        parse_item(fd, &type, &regexp, &string);
        if(type == CMDLINE) {
            new_context = malloc(sizeof(struct rewrite_context));
            if(new_context == NULL) {
                perror("malloc");
                abort();
            } else {
                new_context->cmdline = !strcmp(regexp->raw, "") ? NULL : regexp;
                new_context->rules = last_rule = NULL;
                new_context->next = NULL;
                current_context->next = new_context;
                current_context = new_context;
            }
        } else if(type == RULE) {
            rule = malloc(sizeof(struct rewrite_rule));
            if(rule == NULL) {
                perror("malloc");
                abort();
            }
            
            rule->filename_regexp = regexp;
            rule->rewritten_path = (!strcmp(string, ".")) ? (free(string), NULL) : string;
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
    REWRITE_OPT("-c %s",           config_file, 0),
    REWRITE_OPT("config=%s",       config_file, 0),
    REWRITE_OPT("-v %i",           verbose, 0),
    REWRITE_OPT("verbose=%i",      verbose, 0),

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
        }
        break;

    case KEY_HELP:
        fprintf(stderr,
                "usage: %s source mountpoint [options]\n"
                "\n"
                "general options:\n"
                "    -o opt,[opt...]  mount options\n"
                "    -h   --help      print help\n"
                "    -V   --version   print version\n"
                "\n"
                "rewritefs options:\n"
                "    -c CONFIG        path to configuration file\n"
                "    -r PATH          path to source filesystem\n"
                "    -v LEVEL         verbose level [to be used with -f or -d]\n"
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
    fuse_opt_add_arg(outargs, "use_ino,default_permissions");
    if(config.orig_fs == NULL) {
        fprintf(stderr, "missing source argument\n");
        exit(1);
    } else {
        config.orig_fs = canonicalize_file_name(config.orig_fs);
        if(config.orig_fs[strlen(config.orig_fs)-1] == '/')
            config.orig_fs[strlen(config.orig_fs)-1] = 0;
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
                DEBUG(1, "  \"%s\" -> \"%s\"\n", rule->filename_regexp->raw, rule->rewritten_path ? rule->rewritten_path : "(don't rewrite)");
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
        perror("malloc");
        abort();
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

char *apply_rule(const char *path, struct rewrite_rule *rule) {
    int *ovector, nvec;
    char *rewritten;
    
    if(rule == NULL || rule->rewritten_path == NULL) {
        rewritten = strcat(strcpy(malloc(strlen(config.orig_fs)+strlen(path)+1), config.orig_fs),
                      path);
        DEBUG(2, "  (ignored) %s -> %s\n", path, rewritten);
        DEBUG(3, "\n");
        return rewritten;
    }
    
    /* Fill ovector */
    nvec = (rule->filename_regexp->captures + 1) * 3;
    ovector = calloc(nvec, sizeof(int));
    pcre_exec(rule->filename_regexp->regexp, rule->filename_regexp->extra, path+1,
        strlen(path)-1, 0, 0, ovector, nvec);
    
    /* rewritten = orig_fs + part of path before the matched part + rewritten_path + part of path after the matched path */
    rewritten = malloc(strlen(config.orig_fs) + strlen(rule->rewritten_path) + 1 /* \0 */ + 
        1 + ovector[0] + /* before */
        strlen(path) - ovector[1] /* after */);
    DEBUG(4, "  orig_fs = %s\n",  config.orig_fs);
    DEBUG(4, "  begin = %s\n", strndup(path, ovector[0] + 1));
    DEBUG(4, "  rewritten = %s\n", rule->rewritten_path);
    DEBUG(4, "  end = %s\n", path + 1 + ovector[1]);
    strcpy(rewritten, config.orig_fs);
    strncat(rewritten, path, 1 + ovector[0]);
    strcat(rewritten, rule->rewritten_path); /* XXX replace \1 ... \n if needed */
    strcat(rewritten, path + 1 + ovector[1]);
    
    free(ovector);
    
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
            if(!caller)
                caller = get_caller_cmdline();
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
                DEBUG(3, "    RULE OK \"%s\" \"%s\"\n", rule->filename_regexp->raw, rule->rewritten_path ? rule->rewritten_path : "(don't rewrite)");
                free(caller);
                return apply_rule(path, rule);
            }
        }
    }
    
    free(caller);
    return apply_rule(path, NULL);
}
