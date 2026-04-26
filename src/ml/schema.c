#include "schema.h"

#include <string.h>

/* ── Data model ──────────────────────────────────────────────────────────── */
/* Each opt covers one completion candidate.
 * - name: the full token, e.g. "commit", "--amend", "-m"
 * - subcmd: if non-NULL, opt is only offered when this subcommand is active */

typedef struct {
    const char *name;
    const char *subcmd;
} SchemaOpt;

typedef struct {
    const char       *cmd;
    const SchemaOpt  *opts; /* NULL-sentinel terminated */
} CLISchema;

/* ── git ─────────────────────────────────────────────────────────────────── */
static const SchemaOpt git_opts[] = {
    /* subcommands */
    {"status",       NULL}, {"commit",  NULL}, {"push",    NULL},
    {"pull",         NULL}, {"add",     NULL}, {"checkout",NULL},
    {"diff",         NULL}, {"log",     NULL}, {"branch",  NULL},
    {"stash",        NULL}, {"merge",   NULL}, {"rebase",  NULL},
    {"clone",        NULL}, {"fetch",   NULL}, {"remote",  NULL},
    {"reset",        NULL}, {"restore", NULL}, {"switch",  NULL},
    {"show",         NULL}, {"tag",     NULL}, {"cherry-pick", NULL},
    {"revert",       NULL}, {"bisect",  NULL}, {"mv",      NULL},
    {"rm",           NULL}, {"init",    NULL}, {"submodule",NULL},
    {"worktree",     NULL}, {"config",  NULL}, {"blame",   NULL},
    {"shortlog",     NULL}, {"reflog",  NULL},
    /* global flags */
    {"--version",    NULL}, {"--help",  NULL}, {"--no-pager", NULL},
    {"--bare",       NULL},
    /* git add */
    {"--all",        "add"}, {"--patch",       "add"},
    {"--interactive","add"}, {"--force",       "add"},
    {"--update",     "add"},
    /* git commit */
    {"--message",    "commit"}, {"--amend",      "commit"},
    {"--all",        "commit"}, {"--no-edit",    "commit"},
    {"--signoff",    "commit"}, {"--allow-empty","commit"},
    {"--no-verify",  "commit"}, {"--verbose",    "commit"},
    {"--author",     "commit"}, {"--date",       "commit"},
    {"--fixup",      "commit"}, {"--squash",     "commit"},
    /* git push */
    {"--force",            "push"}, {"--force-with-lease","push"},
    {"--set-upstream",     "push"}, {"--tags",            "push"},
    {"--no-verify",        "push"}, {"--delete",          "push"},
    {"--dry-run",          "push"},
    /* git pull */
    {"--rebase",     "pull"}, {"--no-rebase",  "pull"},
    {"--ff-only",    "pull"}, {"--no-ff",      "pull"},
    {"--autostash",  "pull"},
    /* git log */
    {"--oneline",    "log"}, {"--graph",      "log"},
    {"--all",        "log"}, {"--decorate",   "log"},
    {"--follow",     "log"}, {"--reverse",    "log"},
    {"--stat",       "log"}, {"--patch",      "log"},
    {"--no-merges",  "log"}, {"--since",      "log"},
    {"--until",      "log"}, {"--author",     "log"},
    {"--format",     "log"},
    /* git diff */
    {"--stat",       "diff"}, {"--cached",    "diff"},
    {"--staged",     "diff"}, {"--word-diff", "diff"},
    {"--name-only",  "diff"}, {"--no-index",  "diff"},
    /* git checkout / switch */
    {"--branch",     "checkout"}, {"--track",   "checkout"},
    {"--orphan",     "checkout"}, {"--force",   "checkout"},
    {"--detach",     "checkout"},
    {"--branch",     "switch"},   {"--detach",  "switch"},
    {"--force",      "switch"},   {"--orphan",  "switch"},
    /* git branch */
    {"--delete",           "branch"}, {"--force-delete", "branch"},
    {"--move",             "branch"}, {"--copy",         "branch"},
    {"--all",              "branch"}, {"--remote",       "branch"},
    {"--verbose",          "branch"}, {"--merged",       "branch"},
    {"--no-merged",        "branch"}, {"--set-upstream-to","branch"},
    /* git stash sub-subcommands */
    {"push",     "stash"}, {"pop",    "stash"}, {"apply", "stash"},
    {"list",     "stash"}, {"drop",   "stash"}, {"show",  "stash"},
    {"branch",   "stash"}, {"clear",  "stash"},
    /* git rebase */
    {"--interactive","rebase"}, {"--onto",       "rebase"},
    {"--abort",      "rebase"}, {"--continue",   "rebase"},
    {"--skip",       "rebase"}, {"--autosquash", "rebase"},
    {"--autostash",  "rebase"},
    /* git merge */
    {"--no-ff",    "merge"}, {"--squash",  "merge"},
    {"--abort",    "merge"}, {"--continue","merge"},
    {"--no-commit","merge"}, {"--ff-only", "merge"},
    /* git clone */
    {"--depth",             "clone"}, {"--branch",          "clone"},
    {"--recurse-submodules","clone"}, {"--single-branch",   "clone"},
    {"--no-tags",           "clone"},
    /* git reset */
    {"--soft",  "reset"}, {"--mixed", "reset"},
    {"--hard",  "reset"}, {"--merge", "reset"},
    {"--keep",  "reset"},
    /* git remote subcommands */
    {"add",     "remote"}, {"remove",  "remote"},
    {"rename",  "remote"}, {"set-url", "remote"},
    {"get-url", "remote"}, {"show",    "remote"},
    {"prune",   "remote"},
    {NULL, NULL}
};

/* ── cargo ───────────────────────────────────────────────────────────────── */
static const SchemaOpt cargo_opts[] = {
    /* subcommands */
    {"build",   NULL}, {"run",     NULL}, {"test",    NULL},
    {"check",   NULL}, {"clippy",  NULL}, {"fmt",     NULL},
    {"doc",     NULL}, {"new",     NULL}, {"init",    NULL},
    {"add",     NULL}, {"update",  NULL}, {"install", NULL},
    {"publish", NULL}, {"bench",   NULL}, {"clean",   NULL},
    {"tree",    NULL}, {"search",  NULL}, {"login",   NULL},
    {"logout",  NULL}, {"package", NULL},
    /* build / check / run / test / bench shared flags */
    {"--release",            "build"}, {"--target",             "build"},
    {"--features",           "build"}, {"--all-features",       "build"},
    {"--no-default-features","build"}, {"--verbose",            "build"},
    {"--quiet",              "build"}, {"--jobs",               "build"},
    {"--manifest-path",      "build"},
    {"--release",            "check"}, {"--target",             "check"},
    {"--features",           "check"}, {"--all-features",       "check"},
    {"--no-default-features","check"}, {"--verbose",            "check"},
    {"--release",            "run"},   {"--target",             "run"},
    {"--features",           "run"},   {"--all-features",       "run"},
    {"--no-default-features","run"},   {"--verbose",            "run"},
    {"--release",            "test"},  {"--no-run",             "test"},
    {"--features",           "test"},  {"--all-features",       "test"},
    {"--no-default-features","test"},  {"--lib",                "test"},
    {"--bins",               "test"},  {"--test",               "test"},
    {"--bench",              "test"},  {"--verbose",            "test"},
    /* cargo add */
    {"--dev",      "add"}, {"--features", "add"}, {"--optional","add"},
    {"--path",     "add"}, {"--git",      "add"},
    /* cargo fmt */
    {"--check",  "fmt"}, {"--all", "fmt"},
    /* cargo clippy */
    {"--fix",          "clippy"}, {"--all-targets",       "clippy"},
    {"--all-features", "clippy"}, {"--no-default-features","clippy"},
    /* cargo clean */
    {"--release", "clean"}, {"--target", "clean"},
    {NULL, NULL}
};

/* ── docker ──────────────────────────────────────────────────────────────── */
static const SchemaOpt docker_opts[] = {
    /* subcommands */
    {"run",       NULL}, {"build",   NULL}, {"pull",    NULL},
    {"push",      NULL}, {"ps",      NULL}, {"exec",    NULL},
    {"stop",      NULL}, {"start",   NULL}, {"rm",      NULL},
    {"rmi",       NULL}, {"images",  NULL}, {"logs",    NULL},
    {"inspect",   NULL}, {"network", NULL}, {"volume",  NULL},
    {"compose",   NULL}, {"container",NULL},{"image",   NULL},
    {"system",    NULL}, {"info",    NULL}, {"version", NULL},
    {"tag",       NULL}, {"login",   NULL}, {"logout",  NULL},
    /* docker run */
    {"--rm",          "run"}, {"--interactive",  "run"},
    {"--tty",         "run"}, {"--detach",       "run"},
    {"--volume",      "run"}, {"--env",          "run"},
    {"--publish",     "run"}, {"--name",         "run"},
    {"--network",     "run"}, {"--user",         "run"},
    {"--workdir",     "run"}, {"--entrypoint",   "run"},
    {"--memory",      "run"}, {"--cpus",         "run"},
    {"--restart",     "run"}, {"--privileged",   "run"},
    /* docker build */
    {"--tag",       "build"}, {"--file",        "build"},
    {"--no-cache",  "build"}, {"--build-arg",   "build"},
    {"--target",    "build"}, {"--platform",    "build"},
    {"--quiet",     "build"}, {"--pull",        "build"},
    /* docker exec */
    {"--interactive","exec"}, {"--tty",     "exec"},
    {"--user",       "exec"}, {"--workdir", "exec"},
    {"--env",        "exec"},
    /* docker ps */
    {"--all",    "ps"}, {"--filter", "ps"},
    {"--format", "ps"}, {"--no-trunc","ps"},
    {"--quiet",  "ps"}, {"--size",   "ps"},
    /* docker images */
    {"--all",    "images"}, {"--filter", "images"},
    {"--format", "images"}, {"--no-trunc","images"},
    {"--quiet",  "images"},
    /* docker logs */
    {"--follow",     "logs"}, {"--tail",       "logs"},
    {"--timestamps", "logs"}, {"--since",      "logs"},
    {"--until",      "logs"},
    /* docker compose subcommands */
    {"up",      "compose"}, {"down",    "compose"},
    {"build",   "compose"}, {"logs",    "compose"},
    {"exec",    "compose"}, {"ps",      "compose"},
    {"pull",    "compose"}, {"push",    "compose"},
    {"restart", "compose"}, {"stop",    "compose"},
    {"start",   "compose"}, {"run",     "compose"},
    {"config",  "compose"}, {"images",  "compose"},
    {NULL, NULL}
};

/* ── kubectl ─────────────────────────────────────────────────────────────── */
static const SchemaOpt kubectl_opts[] = {
    /* subcommands */
    {"get",          NULL}, {"apply",       NULL}, {"delete",   NULL},
    {"describe",     NULL}, {"logs",        NULL}, {"exec",     NULL},
    {"port-forward", NULL}, {"create",      NULL}, {"edit",     NULL},
    {"label",        NULL}, {"annotate",    NULL}, {"scale",    NULL},
    {"rollout",      NULL}, {"set",         NULL}, {"config",   NULL},
    {"cluster-info", NULL}, {"top",         NULL}, {"version",  NULL},
    {"patch",        NULL}, {"cp",          NULL}, {"auth",     NULL},
    {"api-resources",NULL}, {"explain",     NULL},
    /* global */
    {"--namespace",       NULL}, {"--context",        NULL},
    {"--kubeconfig",      NULL}, {"--output",         NULL},
    /* kubectl get */
    {"--output",          "get"}, {"--namespace",     "get"},
    {"--all-namespaces",  "get"}, {"--selector",      "get"},
    {"--watch",           "get"}, {"--field-selector","get"},
    {"--show-labels",     "get"},
    /* kubectl apply */
    {"--filename",    "apply"}, {"--recursive",  "apply"},
    {"--dry-run",     "apply"}, {"--namespace",  "apply"},
    {"--server-side", "apply"}, {"--force",      "apply"},
    /* kubectl delete */
    {"--filename",    "delete"}, {"--grace-period","delete"},
    {"--force",       "delete"}, {"--namespace",   "delete"},
    {"--cascade",     "delete"}, {"--all",         "delete"},
    /* kubectl logs */
    {"--follow",      "logs"}, {"--tail",       "logs"},
    {"--previous",    "logs"}, {"--container",  "logs"},
    {"--since",       "logs"}, {"--timestamps", "logs"},
    {"--all-containers","logs"},
    /* kubectl exec */
    {"--stdin",    "exec"}, {"--tty",       "exec"},
    {"--container","exec"}, {"--namespace", "exec"},
    /* kubectl describe */
    {"--namespace", "describe"}, {"--selector", "describe"},
    /* kubectl rollout subcommands */
    {"status",  "rollout"}, {"history", "rollout"},
    {"undo",    "rollout"}, {"pause",   "rollout"},
    {"resume",  "rollout"}, {"restart", "rollout"},
    {NULL, NULL}
};

/* ── npm ─────────────────────────────────────────────────────────────────── */
static const SchemaOpt npm_opts[] = {
    {"install",   NULL}, {"start",    NULL}, {"test",    NULL},
    {"run",       NULL}, {"build",    NULL}, {"init",    NULL},
    {"update",    NULL}, {"uninstall",NULL}, {"publish", NULL},
    {"audit",     NULL}, {"ci",       NULL}, {"exec",    NULL},
    {"link",      NULL}, {"list",     NULL}, {"login",   NULL},
    {"logout",    NULL}, {"outdated", NULL}, {"pack",    NULL},
    {"version",   NULL}, {"view",     NULL}, {"fund",    NULL},
    /* npm install */
    {"--save-dev",        "install"}, {"--global",           "install"},
    {"--production",      "install"}, {"--no-save",          "install"},
    {"--legacy-peer-deps","install"}, {"--force",            "install"},
    {"--no-audit",        "install"},
    /* npm audit */
    {"--fix",   "audit"}, {"--production", "audit"},
    {NULL, NULL}
};

/* ── go ──────────────────────────────────────────────────────────────────── */
static const SchemaOpt go_opts[] = {
    {"build",    NULL}, {"run",     NULL}, {"test",    NULL},
    {"fmt",      NULL}, {"vet",     NULL}, {"mod",     NULL},
    {"get",      NULL}, {"install", NULL}, {"generate",NULL},
    {"clean",    NULL}, {"doc",     NULL}, {"env",     NULL},
    {"version",  NULL}, {"work",    NULL}, {"tool",    NULL},
    /* go build / run */
    {"--output", "build"}, {"-o",        "build"},
    {"-v",       "build"}, {"-race",     "build"},
    {"-tags",    "build"}, {"-ldflags",  "build"},
    {"-v",       "run"},   {"-race",     "run"},
    /* go test */
    {"-v",       "test"},  {"-run",      "test"},
    {"-bench",   "test"},  {"-count",    "test"},
    {"-cover",   "test"},  {"-race",     "test"},
    {"-timeout", "test"},  {"-parallel", "test"},
    /* go mod subcommands */
    {"init",  "mod"}, {"tidy", "mod"}, {"download", "mod"},
    {"verify","mod"}, {"edit", "mod"}, {"vendor",   "mod"},
    {NULL, NULL}
};

/* ── brew ────────────────────────────────────────────────────────────────── */
static const SchemaOpt brew_opts[] = {
    {"install",   NULL}, {"uninstall",NULL}, {"update",  NULL},
    {"upgrade",   NULL}, {"list",     NULL}, {"search",  NULL},
    {"info",      NULL}, {"doctor",   NULL}, {"cleanup", NULL},
    {"tap",       NULL}, {"untap",    NULL}, {"link",    NULL},
    {"unlink",    NULL}, {"services", NULL}, {"deps",    NULL},
    {"uses",      NULL}, {"outdated", NULL}, {"pin",     NULL},
    {"unpin",     NULL}, {"reinstall",NULL}, {"autoremove",NULL},
    /* brew install */
    {"--cask",    "install"}, {"--formula",    "install"},
    {"--force",   "install"}, {"--verbose",    "install"},
    {"--dry-run", "install"},
    /* brew upgrade */
    {"--cask",    "upgrade"}, {"--greedy",     "upgrade"},
    {"--dry-run", "upgrade"},
    {NULL, NULL}
};

/* ── swift ───────────────────────────────────────────────────────────────── */
static const SchemaOpt swift_opts[] = {
    {"build",    NULL}, {"run",     NULL}, {"test",    NULL},
    {"package",  NULL}, {"repl",    NULL},
    /* swift build / run */
    {"--configuration", "build"}, {"--arch",          "build"},
    {"-c",              "build"}, {"--show-bin-path",  "build"},
    {"--configuration", "run"},   {"--arch",          "run"},
    /* swift test */
    {"--parallel", "test"}, {"--filter",      "test"},
    {"--skip",     "test"}, {"--configuration","test"},
    /* swift package subcommands */
    {"init",            "package"}, {"update",    "package"},
    {"resolve",         "package"}, {"edit",      "package"},
    {"unedit",          "package"}, {"show-dependencies","package"},
    {"generate-xcodeproj","package"},
    {NULL, NULL}
};

/* ── pip / pip3 ──────────────────────────────────────────────────────────── */
static const SchemaOpt pip_opts[] = {
    {"install",   NULL}, {"uninstall",NULL}, {"list",    NULL},
    {"show",      NULL}, {"freeze",   NULL}, {"check",   NULL},
    {"download",  NULL}, {"wheel",    NULL}, {"hash",    NULL},
    {"config",    NULL}, {"index",    NULL},
    /* pip install */
    {"--upgrade",        "install"}, {"--user",           "install"},
    {"--requirement",    "install"}, {"--editable",       "install"},
    {"--no-deps",        "install"}, {"--pre",            "install"},
    {"--index-url",      "install"}, {"--extra-index-url","install"},
    {"--force-reinstall","install"},
    {NULL, NULL}
};

/* ── python / python3 ────────────────────────────────────────────────────── */
static const SchemaOpt python_opts[] = {
    {"-m", NULL}, {"-c",         NULL}, {"-i",       NULL},
    {"-V", NULL}, {"--version",  NULL}, {"-u",       NULL},
    {"-O", NULL}, {"-B",         NULL}, {"-W",       NULL},
    {NULL, NULL}
};

/* ── xcodebuild ──────────────────────────────────────────────────────────── */
static const SchemaOpt xcodebuild_opts[] = {
    {"build",   NULL}, {"test",         NULL}, {"clean",  NULL},
    {"archive", NULL}, {"install",      NULL}, {"analyze",NULL},
    {"-project",     NULL}, {"-workspace",NULL},
    {"-scheme",      NULL}, {"-configuration",NULL},
    {"-destination", NULL}, {"-sdk",     NULL},
    {"-showBuildSettings",NULL}, {"-list",NULL},
    {NULL, NULL}
};

/* ── xcrun ───────────────────────────────────────────────────────────────── */
static const SchemaOpt xcrun_opts[] = {
    {"--sdk",            NULL}, {"--find",          NULL},
    {"--show-sdk-path",  NULL}, {"simctl",          NULL},
    {"xctrace",          NULL}, {"clang",           NULL},
    {"clang++",          NULL}, {"swift",           NULL},
    {"swiftc",           NULL}, {"instruments",     NULL},
    {NULL, NULL}
};

/* ── make ────────────────────────────────────────────────────────────────── */
static const SchemaOpt make_opts[] = {
    {"all",     NULL}, {"clean",   NULL}, {"install", NULL},
    {"test",    NULL}, {"build",   NULL}, {"run",     NULL},
    {"help",    NULL}, {"debug",   NULL}, {"release", NULL},
    {"check",   NULL}, {"dist",    NULL}, {"distclean",NULL},
    {"-j",      NULL}, {"-n",      NULL}, {"-k",      NULL},
    {"-f",      NULL}, {"-C",      NULL}, {"-s",      NULL},
    {NULL, NULL}
};

/* ── ssh / scp ───────────────────────────────────────────────────────────── */
static const SchemaOpt ssh_opts[] = {
    {"-p", NULL}, {"-i",          NULL}, {"-L",  NULL},
    {"-R", NULL}, {"-D",          NULL}, {"-N",  NULL},
    {"-T", NULL}, {"-A",          NULL}, {"-v",  NULL},
    {"-X", NULL}, {"-o",          NULL}, {"-4",  NULL},
    {"-6", NULL}, {"-J",          NULL},
    {NULL, NULL}
};

/* ── registry ────────────────────────────────────────────────────────────── */
static const CLISchema g_schemas[] = {
    {"git",        git_opts},
    {"cargo",      cargo_opts},
    {"docker",     docker_opts},
    {"kubectl",    kubectl_opts},
    {"npm",        npm_opts},
    {"npx",        npm_opts},
    {"go",         go_opts},
    {"brew",       brew_opts},
    {"swift",      swift_opts},
    {"pip",        pip_opts},
    {"pip3",       pip_opts},
    {"python",     python_opts},
    {"python3",    python_opts},
    {"xcodebuild", xcodebuild_opts},
    {"xcrun",      xcrun_opts},
    {"make",       make_opts},
    {"gmake",      make_opts},
    {"ssh",        ssh_opts},
    {NULL, NULL}
};

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Split `s` into space-delimited tokens; returns count, fills tok/tlen. */
static int tokenise(const char *s, const char **tok, size_t *tlen, int cap)
{
    int n = 0;
    while (*s && n < cap) {
        while (' ' == *s || '\t' == *s) s++;
        if (!*s) break;
        tok[n]  = s;
        tlen[n] = 0;
        while (*s && ' ' != *s && '\t' != *s) { s++; tlen[n]++; }
        n++;
    }
    return n;
}

/* ── public API ──────────────────────────────────────────────────────────── */
int schema_complete(const char *query, char *out, size_t cap)
{
    if (!query || !out || cap < 2) return -1;
    out[0] = '\0';

    /* Tokenise, keeping the raw trailing-space flag. */
    const char *tok[16];
    size_t      tlen[16];
    int         n = tokenise(query, tok, tlen, 16);
    if (n < 1) return -1;

    /* Detect whether query ends with a space (partial = ""). */
    const char *end   = query + strlen(query);
    int         trail = (end > query && (' ' == end[-1] || '\t' == end[-1]));

    /* cmd must be the first complete token. */
    if (trail && n < 1) return -1;
    if (!trail && n < 2) return -1; /* need at least cmd + partial */

    const char *cmd  = tok[0];
    size_t      clen = tlen[0];

    /* Find schema entry. */
    const CLISchema *schema = NULL;
    for (int i = 0; g_schemas[i].cmd; i++) {
        if (strlen(g_schemas[i].cmd) == clen &&
            0 == strncmp(g_schemas[i].cmd, cmd, clen)) {
            schema = &g_schemas[i];
            break;
        }
    }
    if (!schema) return -1;

    /* Extract partial (last token if no trailing space, else ""). */
    const char *partial;
    size_t      plen;
    if (trail) {
        partial = "";
        plen    = 0;
    } else {
        partial = tok[n - 1];
        plen    = tlen[n - 1];
    }

    /* Detect active subcommand: first non-flag, non-cmd token before partial.
     * Copied into a null-terminated buffer so strcmp works correctly. */
    char   active_sub[64] = {'\0'};
    int    middle_end     = trail ? n : n - 1;
    for (int i = 1; i < middle_end; i++) {
        if (tlen[i] > 0 && '-' != tok[i][0]) {
            size_t copy = tlen[i] < sizeof active_sub - 1
                          ? tlen[i] : sizeof active_sub - 1;
            memcpy(active_sub, tok[i], copy);
            active_sub[copy] = '\0';
            break;
        }
    }

    /* Determine match mode: flag (partial starts with '-') or subcommand. */
    int flag_mode = (plen > 0 && '-' == partial[0]);

    /* Scan opts: compute LCP of all matching suffixes. */
    char   lcp[256];
    size_t lcp_len  = 0;
    int    n_match  = 0;

    for (int j = 0; schema->opts[j].name; j++) {
        const char *name     = schema->opts[j].name;
        const char *opt_sub  = schema->opts[j].subcmd;
        size_t      name_len = strlen(name);

        if (flag_mode) {
            /* Flag mode: offer flags that start with '-'.
             * Include subcmd-specific flags AND global (opt_sub=NULL) flags. */
            if ('-' != name[0]) continue;
            if (opt_sub) {
                if ('\0' == active_sub[0]) continue;
                if (0 != strcmp(opt_sub, active_sub)) continue;
            }
        } else {
            /* Subcommand/positional mode. */
            if ('-' == name[0]) continue;
            if ('\0' != active_sub[0]) {
                /* Active subcmd set: only its sub-subcommands. */
                if (!opt_sub || 0 != strcmp(opt_sub, active_sub)) continue;
            } else {
                /* No active subcmd: top-level subcommands only. */
                if (opt_sub) continue;
            }
        }

        /* Prefix match against partial. */
        if (name_len < plen) continue;
        if (0 != strncmp(name, partial, plen)) continue;

        /* Update LCP. */
        if (0 == n_match) {
            size_t copy = name_len < sizeof lcp - 1 ? name_len : sizeof lcp - 1;
            memcpy(lcp, name, copy);
            lcp[copy] = '\0';
            lcp_len   = name_len;
        } else {
            size_t i = 0;
            while (i < lcp_len && i < name_len && lcp[i] == name[i]) i++;
            lcp_len = i;
        }
        n_match++;
    }

    if (0 == n_match) return -1;

    /* Suffix = LCP past the partial prefix. */
    size_t suffix_len = lcp_len - plen;
    if (n_match > 1 && 0 == suffix_len) return -1;

    if (suffix_len >= cap) suffix_len = cap - 1;
    memcpy(out, lcp + plen, suffix_len);
    out[suffix_len] = '\0';
    return (int)suffix_len;
}
