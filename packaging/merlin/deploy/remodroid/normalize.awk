# Validate INI syntax and rewrite host-specific agent paths for Merlin.
function paths() {
    print "files_dir = /koolshare/remodroid/phone"
    print "adbkey = /jffs/configs/remodroid/adbkey"
    print "socket = /tmp/remodroid/agent.sock"
}
{
    sub(/\r$/, "")
    if (length($0)>1000) {bad=1; exit}
    if ($0 ~ /^[ \t]*\[/) {
        if ($0 !~ /^[ \t]*\[[A-Za-z0-9_]+\][ \t]*([;#].*)?$/) {bad=1; exit}
        section=$0; sub(/^[ \t]*\[/,"",section); sub(/\].*$/, "",section)
        if (seen[section]++) {bad=1; exit}
        print
        if (section=="agent") paths()
        next
    }
    if ($0 ~ /^[ \t]*([;#]|$)/) {print; next}
    if (section=="" || $0 !~ /^[ \t]*[A-Za-z0-9_]+[ \t]*=/) {bad=1; exit}
    key=$0; sub(/=.*/,"",key); gsub(/^[ \t]+|[ \t]+$/,"",key)
    if (keys[section SUBSEP key]++) {bad=1; exit}
    if (section=="agent" && (key=="files_dir" || key=="adbkey" || key=="socket")) next
    print
}
END {if (!seen["server"] || !seen["agent"] || bad) exit 1}
