#include "schema.h"

#include <string.h>

/* subcmd: non-NULL restricts opt to that subcommand context */
typedef struct {
    const char *name;
    const char *subcmd;
} SchemaOpt;

typedef struct {
    const char *cmd;
    const SchemaOpt *opts; /* NULL-sentinel terminated */
} CLISchema;

static const SchemaOpt git_opts[] = {{"status", NULL},
                                     {"commit", NULL},
                                     {"push", NULL},
                                     {"pull", NULL},
                                     {"add", NULL},
                                     {"checkout", NULL},
                                     {"diff", NULL},
                                     {"log", NULL},
                                     {"branch", NULL},
                                     {"stash", NULL},
                                     {"merge", NULL},
                                     {"rebase", NULL},
                                     {"clone", NULL},
                                     {"fetch", NULL},
                                     {"remote", NULL},
                                     {"reset", NULL},
                                     {"restore", NULL},
                                     {"switch", NULL},
                                     {"show", NULL},
                                     {"tag", NULL},
                                     {"cherry-pick", NULL},
                                     {"revert", NULL},
                                     {"bisect", NULL},
                                     {"mv", NULL},
                                     {"rm", NULL},
                                     {"init", NULL},
                                     {"submodule", NULL},
                                     {"worktree", NULL},
                                     {"config", NULL},
                                     {"blame", NULL},
                                     {"shortlog", NULL},
                                     {"reflog", NULL},
                                     {"--version", NULL},
                                     {"--help", NULL},
                                     {"--no-pager", NULL},
                                     {"--bare", NULL},
                                     {"--all", "add"},
                                     {"--patch", "add"},
                                     {"--interactive", "add"},
                                     {"--force", "add"},
                                     {"--update", "add"},
                                     {"--message", "commit"},
                                     {"--amend", "commit"},
                                     {"--all", "commit"},
                                     {"--no-edit", "commit"},
                                     {"--signoff", "commit"},
                                     {"--allow-empty", "commit"},
                                     {"--no-verify", "commit"},
                                     {"--verbose", "commit"},
                                     {"--author", "commit"},
                                     {"--date", "commit"},
                                     {"--fixup", "commit"},
                                     {"--squash", "commit"},
                                     {"--force", "push"},
                                     {"--force-with-lease", "push"},
                                     {"--set-upstream", "push"},
                                     {"--tags", "push"},
                                     {"--no-verify", "push"},
                                     {"--delete", "push"},
                                     {"--dry-run", "push"},
                                     {"--rebase", "pull"},
                                     {"--no-rebase", "pull"},
                                     {"--ff-only", "pull"},
                                     {"--no-ff", "pull"},
                                     {"--autostash", "pull"},
                                     {"--oneline", "log"},
                                     {"--graph", "log"},
                                     {"--all", "log"},
                                     {"--decorate", "log"},
                                     {"--follow", "log"},
                                     {"--reverse", "log"},
                                     {"--stat", "log"},
                                     {"--patch", "log"},
                                     {"--no-merges", "log"},
                                     {"--since", "log"},
                                     {"--until", "log"},
                                     {"--author", "log"},
                                     {"--format", "log"},
                                     {"--stat", "diff"},
                                     {"--cached", "diff"},
                                     {"--staged", "diff"},
                                     {"--word-diff", "diff"},
                                     {"--name-only", "diff"},
                                     {"--no-index", "diff"},
                                     {"--branch", "checkout"},
                                     {"--track", "checkout"},
                                     {"--orphan", "checkout"},
                                     {"--force", "checkout"},
                                     {"--detach", "checkout"},
                                     {"--branch", "switch"},
                                     {"--detach", "switch"},
                                     {"--force", "switch"},
                                     {"--orphan", "switch"},
                                     {"--delete", "branch"},
                                     {"--force-delete", "branch"},
                                     {"--move", "branch"},
                                     {"--copy", "branch"},
                                     {"--all", "branch"},
                                     {"--remote", "branch"},
                                     {"--verbose", "branch"},
                                     {"--merged", "branch"},
                                     {"--no-merged", "branch"},
                                     {"--set-upstream-to", "branch"},
                                     {"push", "stash"},
                                     {"pop", "stash"},
                                     {"apply", "stash"},
                                     {"list", "stash"},
                                     {"drop", "stash"},
                                     {"show", "stash"},
                                     {"branch", "stash"},
                                     {"clear", "stash"},
                                     {"--interactive", "rebase"},
                                     {"--onto", "rebase"},
                                     {"--abort", "rebase"},
                                     {"--continue", "rebase"},
                                     {"--skip", "rebase"},
                                     {"--autosquash", "rebase"},
                                     {"--autostash", "rebase"},
                                     {"--no-ff", "merge"},
                                     {"--squash", "merge"},
                                     {"--abort", "merge"},
                                     {"--continue", "merge"},
                                     {"--no-commit", "merge"},
                                     {"--ff-only", "merge"},
                                     {"--depth", "clone"},
                                     {"--branch", "clone"},
                                     {"--recurse-submodules", "clone"},
                                     {"--single-branch", "clone"},
                                     {"--no-tags", "clone"},
                                     {"--soft", "reset"},
                                     {"--mixed", "reset"},
                                     {"--hard", "reset"},
                                     {"--merge", "reset"},
                                     {"--keep", "reset"},
                                     {"add", "remote"},
                                     {"remove", "remote"},
                                     {"rename", "remote"},
                                     {"set-url", "remote"},
                                     {"get-url", "remote"},
                                     {"show", "remote"},
                                     {"prune", "remote"},
                                     {NULL, NULL}};

static const SchemaOpt cargo_opts[] = {{"build", NULL},
                                       {"run", NULL},
                                       {"test", NULL},
                                       {"check", NULL},
                                       {"clippy", NULL},
                                       {"fmt", NULL},
                                       {"doc", NULL},
                                       {"new", NULL},
                                       {"init", NULL},
                                       {"add", NULL},
                                       {"update", NULL},
                                       {"install", NULL},
                                       {"publish", NULL},
                                       {"bench", NULL},
                                       {"clean", NULL},
                                       {"tree", NULL},
                                       {"search", NULL},
                                       {"login", NULL},
                                       {"logout", NULL},
                                       {"package", NULL},
                                       {"--release", "build"},
                                       {"--target", "build"},
                                       {"--features", "build"},
                                       {"--all-features", "build"},
                                       {"--no-default-features", "build"},
                                       {"--verbose", "build"},
                                       {"--quiet", "build"},
                                       {"--jobs", "build"},
                                       {"--manifest-path", "build"},
                                       {"--release", "check"},
                                       {"--target", "check"},
                                       {"--features", "check"},
                                       {"--all-features", "check"},
                                       {"--no-default-features", "check"},
                                       {"--verbose", "check"},
                                       {"--release", "run"},
                                       {"--target", "run"},
                                       {"--features", "run"},
                                       {"--all-features", "run"},
                                       {"--no-default-features", "run"},
                                       {"--verbose", "run"},
                                       {"--release", "test"},
                                       {"--no-run", "test"},
                                       {"--features", "test"},
                                       {"--all-features", "test"},
                                       {"--no-default-features", "test"},
                                       {"--lib", "test"},
                                       {"--bins", "test"},
                                       {"--test", "test"},
                                       {"--bench", "test"},
                                       {"--verbose", "test"},
                                       {"--dev", "add"},
                                       {"--features", "add"},
                                       {"--optional", "add"},
                                       {"--path", "add"},
                                       {"--git", "add"},
                                       {"--check", "fmt"},
                                       {"--all", "fmt"},
                                       {"--fix", "clippy"},
                                       {"--all-targets", "clippy"},
                                       {"--all-features", "clippy"},
                                       {"--no-default-features", "clippy"},
                                       {"--release", "clean"},
                                       {"--target", "clean"},
                                       {NULL, NULL}};

static const SchemaOpt docker_opts[] = {{"run", NULL},
                                        {"build", NULL},
                                        {"pull", NULL},
                                        {"push", NULL},
                                        {"ps", NULL},
                                        {"exec", NULL},
                                        {"stop", NULL},
                                        {"start", NULL},
                                        {"rm", NULL},
                                        {"rmi", NULL},
                                        {"images", NULL},
                                        {"logs", NULL},
                                        {"inspect", NULL},
                                        {"network", NULL},
                                        {"volume", NULL},
                                        {"compose", NULL},
                                        {"container", NULL},
                                        {"image", NULL},
                                        {"system", NULL},
                                        {"info", NULL},
                                        {"version", NULL},
                                        {"tag", NULL},
                                        {"login", NULL},
                                        {"logout", NULL},
                                        {"--rm", "run"},
                                        {"--interactive", "run"},
                                        {"--tty", "run"},
                                        {"--detach", "run"},
                                        {"--volume", "run"},
                                        {"--env", "run"},
                                        {"--publish", "run"},
                                        {"--name", "run"},
                                        {"--network", "run"},
                                        {"--user", "run"},
                                        {"--workdir", "run"},
                                        {"--entrypoint", "run"},
                                        {"--memory", "run"},
                                        {"--cpus", "run"},
                                        {"--restart", "run"},
                                        {"--privileged", "run"},
                                        {"--tag", "build"},
                                        {"--file", "build"},
                                        {"--no-cache", "build"},
                                        {"--build-arg", "build"},
                                        {"--target", "build"},
                                        {"--platform", "build"},
                                        {"--quiet", "build"},
                                        {"--pull", "build"},
                                        {"--interactive", "exec"},
                                        {"--tty", "exec"},
                                        {"--user", "exec"},
                                        {"--workdir", "exec"},
                                        {"--env", "exec"},
                                        {"--all", "ps"},
                                        {"--filter", "ps"},
                                        {"--format", "ps"},
                                        {"--no-trunc", "ps"},
                                        {"--quiet", "ps"},
                                        {"--size", "ps"},
                                        {"--all", "images"},
                                        {"--filter", "images"},
                                        {"--format", "images"},
                                        {"--no-trunc", "images"},
                                        {"--quiet", "images"},
                                        {"--follow", "logs"},
                                        {"--tail", "logs"},
                                        {"--timestamps", "logs"},
                                        {"--since", "logs"},
                                        {"--until", "logs"},
                                        {"up", "compose"},
                                        {"down", "compose"},
                                        {"build", "compose"},
                                        {"logs", "compose"},
                                        {"exec", "compose"},
                                        {"ps", "compose"},
                                        {"pull", "compose"},
                                        {"push", "compose"},
                                        {"restart", "compose"},
                                        {"stop", "compose"},
                                        {"start", "compose"},
                                        {"run", "compose"},
                                        {"config", "compose"},
                                        {"images", "compose"},
                                        {NULL, NULL}};

static const SchemaOpt kubectl_opts[] = {{"get", NULL},
                                         {"apply", NULL},
                                         {"delete", NULL},
                                         {"describe", NULL},
                                         {"logs", NULL},
                                         {"exec", NULL},
                                         {"port-forward", NULL},
                                         {"create", NULL},
                                         {"edit", NULL},
                                         {"label", NULL},
                                         {"annotate", NULL},
                                         {"scale", NULL},
                                         {"rollout", NULL},
                                         {"set", NULL},
                                         {"config", NULL},
                                         {"cluster-info", NULL},
                                         {"top", NULL},
                                         {"version", NULL},
                                         {"patch", NULL},
                                         {"cp", NULL},
                                         {"auth", NULL},
                                         {"api-resources", NULL},
                                         {"explain", NULL},
                                         {"--namespace", NULL},
                                         {"--context", NULL},
                                         {"--kubeconfig", NULL},
                                         {"--output", NULL},
                                         {"--output", "get"},
                                         {"--namespace", "get"},
                                         {"--all-namespaces", "get"},
                                         {"--selector", "get"},
                                         {"--watch", "get"},
                                         {"--field-selector", "get"},
                                         {"--show-labels", "get"},
                                         {"--filename", "apply"},
                                         {"--recursive", "apply"},
                                         {"--dry-run", "apply"},
                                         {"--namespace", "apply"},
                                         {"--server-side", "apply"},
                                         {"--force", "apply"},
                                         {"--filename", "delete"},
                                         {"--grace-period", "delete"},
                                         {"--force", "delete"},
                                         {"--namespace", "delete"},
                                         {"--cascade", "delete"},
                                         {"--all", "delete"},
                                         {"--follow", "logs"},
                                         {"--tail", "logs"},
                                         {"--previous", "logs"},
                                         {"--container", "logs"},
                                         {"--since", "logs"},
                                         {"--timestamps", "logs"},
                                         {"--all-containers", "logs"},
                                         {"--stdin", "exec"},
                                         {"--tty", "exec"},
                                         {"--container", "exec"},
                                         {"--namespace", "exec"},
                                         {"--namespace", "describe"},
                                         {"--selector", "describe"},
                                         {"status", "rollout"},
                                         {"history", "rollout"},
                                         {"undo", "rollout"},
                                         {"pause", "rollout"},
                                         {"resume", "rollout"},
                                         {"restart", "rollout"},
                                         {NULL, NULL}};

static const SchemaOpt npm_opts[] = {{"install", NULL},
                                     {"start", NULL},
                                     {"test", NULL},
                                     {"run", NULL},
                                     {"build", NULL},
                                     {"init", NULL},
                                     {"update", NULL},
                                     {"uninstall", NULL},
                                     {"publish", NULL},
                                     {"audit", NULL},
                                     {"ci", NULL},
                                     {"exec", NULL},
                                     {"link", NULL},
                                     {"list", NULL},
                                     {"login", NULL},
                                     {"logout", NULL},
                                     {"outdated", NULL},
                                     {"pack", NULL},
                                     {"version", NULL},
                                     {"view", NULL},
                                     {"fund", NULL},
                                     {"--save-dev", "install"},
                                     {"--global", "install"},
                                     {"--production", "install"},
                                     {"--no-save", "install"},
                                     {"--legacy-peer-deps", "install"},
                                     {"--force", "install"},
                                     {"--no-audit", "install"},
                                     {"--fix", "audit"},
                                     {"--production", "audit"},
                                     {NULL, NULL}};

static const SchemaOpt go_opts[] = {
    {"build", NULL},       {"run", NULL},      {"test", NULL},
    {"fmt", NULL},         {"vet", NULL},      {"mod", NULL},
    {"get", NULL},         {"install", NULL},  {"generate", NULL},
    {"clean", NULL},       {"doc", NULL},      {"env", NULL},
    {"version", NULL},     {"work", NULL},     {"tool", NULL},
    {"--output", "build"}, {"-o", "build"},    {"-v", "build"},
    {"-race", "build"},    {"-tags", "build"}, {"-ldflags", "build"},
    {"-v", "run"},         {"-race", "run"},   {"-v", "test"},
    {"-run", "test"},      {"-bench", "test"}, {"-count", "test"},
    {"-cover", "test"},    {"-race", "test"},  {"-timeout", "test"},
    {"-parallel", "test"}, {"init", "mod"},    {"tidy", "mod"},
    {"download", "mod"},   {"verify", "mod"},  {"edit", "mod"},
    {"vendor", "mod"},     {NULL, NULL}};

static const SchemaOpt brew_opts[] = {
    {"install", NULL},        {"uninstall", NULL},
    {"update", NULL},         {"upgrade", NULL},
    {"list", NULL},           {"search", NULL},
    {"info", NULL},           {"doctor", NULL},
    {"cleanup", NULL},        {"tap", NULL},
    {"untap", NULL},          {"link", NULL},
    {"unlink", NULL},         {"services", NULL},
    {"deps", NULL},           {"uses", NULL},
    {"outdated", NULL},       {"pin", NULL},
    {"unpin", NULL},          {"reinstall", NULL},
    {"autoremove", NULL},     {"--cask", "install"},
    {"--formula", "install"}, {"--force", "install"},
    {"--verbose", "install"}, {"--dry-run", "install"},
    {"--cask", "upgrade"},    {"--greedy", "upgrade"},
    {"--dry-run", "upgrade"}, {NULL, NULL}};

static const SchemaOpt swift_opts[] = {{"build", NULL},
                                       {"run", NULL},
                                       {"test", NULL},
                                       {"package", NULL},
                                       {"repl", NULL},
                                       {"--configuration", "build"},
                                       {"--arch", "build"},
                                       {"-c", "build"},
                                       {"--show-bin-path", "build"},
                                       {"--configuration", "run"},
                                       {"--arch", "run"},
                                       {"--parallel", "test"},
                                       {"--filter", "test"},
                                       {"--skip", "test"},
                                       {"--configuration", "test"},
                                       {"init", "package"},
                                       {"update", "package"},
                                       {"resolve", "package"},
                                       {"edit", "package"},
                                       {"unedit", "package"},
                                       {"show-dependencies", "package"},
                                       {"generate-xcodeproj", "package"},
                                       {NULL, NULL}};

static const SchemaOpt pip_opts[] = {{"install", NULL},
                                     {"uninstall", NULL},
                                     {"list", NULL},
                                     {"show", NULL},
                                     {"freeze", NULL},
                                     {"check", NULL},
                                     {"download", NULL},
                                     {"wheel", NULL},
                                     {"hash", NULL},
                                     {"config", NULL},
                                     {"index", NULL},
                                     {"--upgrade", "install"},
                                     {"--user", "install"},
                                     {"--requirement", "install"},
                                     {"--editable", "install"},
                                     {"--no-deps", "install"},
                                     {"--pre", "install"},
                                     {"--index-url", "install"},
                                     {"--extra-index-url", "install"},
                                     {"--force-reinstall", "install"},
                                     {NULL, NULL}};

static const SchemaOpt python_opts[] = {
    {"-m", NULL}, {"-c", NULL}, {"-i", NULL}, {"-V", NULL}, {"--version", NULL},
    {"-u", NULL}, {"-O", NULL}, {"-B", NULL}, {"-W", NULL}, {NULL, NULL}};

static const SchemaOpt xcodebuild_opts[] = {{"build", NULL},
                                            {"test", NULL},
                                            {"clean", NULL},
                                            {"archive", NULL},
                                            {"install", NULL},
                                            {"analyze", NULL},
                                            {"-project", NULL},
                                            {"-workspace", NULL},
                                            {"-scheme", NULL},
                                            {"-configuration", NULL},
                                            {"-destination", NULL},
                                            {"-sdk", NULL},
                                            {"-showBuildSettings", NULL},
                                            {"-list", NULL},
                                            {NULL, NULL}};

static const SchemaOpt xcrun_opts[] = {
    {"--sdk", NULL},       {"--find", NULL},  {"--show-sdk-path", NULL},
    {"simctl", NULL},      {"xctrace", NULL}, {"clang", NULL},
    {"clang++", NULL},     {"swift", NULL},   {"swiftc", NULL},
    {"instruments", NULL}, {NULL, NULL}};

static const SchemaOpt make_opts[] = {
    {"all", NULL},     {"clean", NULL}, {"install", NULL}, {"test", NULL},
    {"build", NULL},   {"run", NULL},   {"help", NULL},    {"debug", NULL},
    {"release", NULL}, {"check", NULL}, {"dist", NULL},    {"distclean", NULL},
    {"-j", NULL},      {"-n", NULL},    {"-k", NULL},      {"-f", NULL},
    {"-C", NULL},      {"-s", NULL},    {NULL, NULL}};

static const SchemaOpt ssh_opts[] = {
    {"-p", NULL}, {"-i", NULL}, {"-L", NULL}, {"-R", NULL}, {"-D", NULL},
    {"-N", NULL}, {"-T", NULL}, {"-A", NULL}, {"-v", NULL}, {"-X", NULL},
    {"-o", NULL}, {"-4", NULL}, {"-6", NULL}, {"-J", NULL}, {NULL, NULL}};

static const CLISchema g_schemas[] = {{"git", git_opts},
                                      {"cargo", cargo_opts},
                                      {"docker", docker_opts},
                                      {"kubectl", kubectl_opts},
                                      {"npm", npm_opts},
                                      {"npx", npm_opts},
                                      {"go", go_opts},
                                      {"brew", brew_opts},
                                      {"swift", swift_opts},
                                      {"pip", pip_opts},
                                      {"pip3", pip_opts},
                                      {"python", python_opts},
                                      {"python3", python_opts},
                                      {"xcodebuild", xcodebuild_opts},
                                      {"xcrun", xcrun_opts},
                                      {"make", make_opts},
                                      {"gmake", make_opts},
                                      {"ssh", ssh_opts},
                                      {NULL, NULL}};

#define TOKEN_CAP 16

/* Splits s into space/tab-delimited tokens; fills tok[]/tlen[], max cap. */
static int tokenise(const char *s, const char **tok, size_t *tlen, int cap) {
    int n = 0;
    while (*s && n < cap) {
        while (' ' == *s || '\t' == *s)
            s++;
        if (!*s)
            break;
        tok[n] = s;
        tlen[n] = 0;
        while (*s && ' ' != *s && '\t' != *s) {
            s++;
            tlen[n]++;
        }
        n++;
    }
    return n;
}

int schema_complete(const char *query, char *out, size_t cap) {
    if (!query || !out || cap < 2)
        return -1;
    out[0] = '\0';

    const char *tok[TOKEN_CAP];
    size_t tlen[TOKEN_CAP];
    int n = tokenise(query, tok, tlen, TOKEN_CAP);
    if (n < 1)
        return -1;

    /* Trailing space means partial is ""; otherwise last token is partial. */
    const char *end = query + strlen(query);
    int trail = (end > query && (' ' == end[-1] || '\t' == end[-1]));

    if (!trail && n < 2)
        return -1;

    const char *cmd = tok[0];
    size_t clen = tlen[0];

    const CLISchema *schema = NULL;
    for (int i = 0; g_schemas[i].cmd; i++) {
        if (strlen(g_schemas[i].cmd) == clen &&
            0 == strncmp(g_schemas[i].cmd, cmd, clen)) {
            schema = &g_schemas[i];
            break;
        }
    }
    if (!schema)
        return -1;

    const char *partial;
    size_t plen;
    if (trail) {
        partial = "";
        plen = 0;
    } else {
        partial = tok[n - 1];
        plen = tlen[n - 1];
    }

    /* First non-flag token after cmd; NUL-terminated so strcmp is safe. */
    char active_sub[64] = {'\0'};
    int middle_end = trail ? n : n - 1;
    for (int i = 1; i < middle_end; i++) {
        if (tlen[i] > 0 && '-' != tok[i][0]) {
            size_t copy = tlen[i] < sizeof active_sub - 1
                              ? tlen[i]
                              : sizeof active_sub - 1;
            memcpy(active_sub, tok[i], copy);
            active_sub[copy] = '\0';
            break;
        }
    }

    int flag_mode = (plen > 0 && '-' == partial[0]);

    /* LCP over all prefix-matching candidates. */
    char lcp[256];
    size_t lcp_len = 0;
    int n_match = 0;

    for (int j = 0; schema->opts[j].name; j++) {
        const char *name = schema->opts[j].name;
        const char *opt_sub = schema->opts[j].subcmd;
        size_t name_len = strlen(name);

        if (flag_mode) {
            /* Global flags (opt_sub=NULL) are offered alongside subcmd flags.
             */
            if ('-' != name[0])
                continue;
            if (opt_sub) {
                if ('\0' == active_sub[0])
                    continue;
                if (0 != strcmp(opt_sub, active_sub))
                    continue;
            }
        } else {
            if ('-' == name[0])
                continue;
            if ('\0' != active_sub[0]) {
                /* Subcmd active: restrict to its sub-subcommands. */
                if (!opt_sub || 0 != strcmp(opt_sub, active_sub))
                    continue;
            } else {
                /* No subcmd yet: top-level only. */
                if (opt_sub)
                    continue;
            }
        }

        if (name_len < plen)
            continue;
        if (0 != strncmp(name, partial, plen))
            continue;

        if (0 == n_match) {
            size_t copy = name_len < sizeof lcp - 1 ? name_len : sizeof lcp - 1;
            memcpy(lcp, name, copy);
            lcp[copy] = '\0';
            lcp_len = name_len;
        } else {
            size_t i = 0;
            while (i < lcp_len && i < name_len && lcp[i] == name[i])
                i++;
            lcp_len = i;
        }
        n_match++;
    }

    if (0 == n_match)
        return -1;

    size_t suffix_len = lcp_len - plen;
    if (n_match > 1 && 0 == suffix_len)
        return -1;

    if (suffix_len >= cap)
        suffix_len = cap - 1;
    memcpy(out, lcp + plen, suffix_len);
    out[suffix_len] = '\0';
    return (int)suffix_len;
}
