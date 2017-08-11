TESTDIR="$BATS_TMPDIR/rewritefs-test-mount"
CFGFILE="$BATS_TMPDIR/rewritefs-test-config"

# Layout of source dir for tests is:
#  foo/
#  foo/bar: text file containing "bar"
#  egg: text file containing "egg"
#  tmp/ directory where we can write stuff. Cleaned after tests.

setup() {
    if [ ! -d "$TESTDIR" ] ; then
         mkdir "$TESTDIR"
    fi
    if [ ! -d "$BATS_TEST_DIRNAME/source/tmp" ] ; then
        mkdir "$BATS_TEST_DIRNAME/source/tmp" 
    fi
}

teardown() {
    if [ -f "$TESTDIR/egg" ] ; then
        fusermount -u "$TESTDIR"
    fi
    rm -rf "$BATS_TEST_DIRNAME/source/tmp" 
}

mount_rewritefs() {
    "$BATS_TEST_DIRNAME/../rewritefs" -r "$BATS_TEST_DIRNAME/source" -c "$CFGFILE" "$TESTDIR"
}

@test "Test simple rules" {
    cat > "$CFGFILE" << EOF
m:^test1: egg
m:^test2(?=/|$): foo
m:test3$: bar
m:test4: oo
EOF
    mount_rewritefs

    run cat "$TESTDIR/test1"
    [ "$status" = 0 ]
    [ "$output" = "egg" ]

    run cat "$TESTDIR/test2/bar"
    [ "$status" = 0 ]
    [ "$output" = "bar" ]

    run cat "$TESTDIR/foo/test3"
    [ "$status" = 0 ]
    [ "$output" = "bar" ]

    run cat "$TESTDIR/ftest4/bar"
    [ "$status" = 0 ]
    [ "$output" = "bar" ]
}

@test "Test escape" {
    cat > "$CFGFILE" << EOF
# double \ because bash will do a first escape. It will be a single \ in the configuration file.
# Escaping char in tests sucks.
/^foo\\/test/ foo/bar
EOF

    mount_rewritefs

    run cat "$TESTDIR/foo/test"
    [ "$status" = 0 ]
    [ "$output" = "bar" ]
}

@test "Test extended regexp" {
    cat > "$CFGFILE" << EOF
m:
    # All properly quoted strings should reference foo/
    # Escape char is %. \ is too painful to work with.
    ^ "
    (?>
        [^%"]++ |
        %.
    )*
    "
:x foo
EOF

    mount_rewritefs

    run cat "$TESTDIR"/'"Hello world"'/bar
    [ "$status" = 0 ]
    [ "$output" = "bar" ]
}

@test "Test lookaround" {
    cat > "$CFGFILE" << EOF
m:^(?=bar): foo/
EOF

    mount_rewritefs

    run cat "$TESTDIR/bar"
    [ "$status" = 0 ]
    [ "$output" = "bar" ]
}

@test "Test backreference" {
    cat > "$CFGFILE" << EOF
m:^f([0-9])\\1: foo
EOF

    mount_rewritefs

    run cat "$TESTDIR/f11/bar"
    [ "$status" = 0 ]
    [ "$output" = "bar" ]
}

@test "Test break" {
    cat > "$CFGFILE" << EOF
/^eg/ .
/^egg/ foo/bar
EOF

    mount_rewritefs

    run cat "$TESTDIR/egg"
    [ "$status" = 0 ]
    [ "$output" = "egg" ]
}

@test "Test backreferences in replace part" {
    cat > "$CFGFILE" << EOF
/^bar-(.+)/ f\\1/bar
/^(.+)(_)(.+)/ \\3/\\1
/^F(.)/ f\\1\\1
EOF

    mount_rewritefs

    run cat "$TESTDIR/bar-oo"
    [ "$status" = 0 ]
    [ "$output" = "bar" ]

    run cat "$TESTDIR/bar_foo"
    [ "$status" = 0 ]
    [ "$output" = "bar" ]

    run cat "$TESTDIR/Fo/bar"
    [ "$status" = 0 ]
    [ "$output" = "bar" ]
}
