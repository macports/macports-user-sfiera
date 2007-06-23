# Test file for registry::item
# Syntax:
# tclsh item.tcl <Pextlib name>

proc main {pextlibname} {
    load $pextlibname

	file delete -force test.db

    check_throws {registry::entry search}
    registry::open test.db

    set vim1 [registry::entry create vim 7.1.000 0 {multibyte +} 0]
    set vim2 [registry::entry create vim 7.1.002 0 {} 0]
    set vim3 [registry::entry create vim 7.1.002 0 {multibyte +} 0]
    set zlib [registry::entry create zlib 1.2.3 1 {} 0]
    set pcre [registry::entry create pcre 7.1 1 {utf8 +} 0]

    $vim1 state installed
    $vim2 state installed
    $vim3 state active
    $zlib state active
    $pcre state installed

    test_equal {[$vim1 name]} vim
    test_equal {[$vim2 epoch]} 0
    test_equal {[$vim3 version]} 7.1.002
    test_equal {[$zlib revision]} 1
    test_equal {[$pcre variants]} {utf8 +}
    
    set installed [registry::entry installed]
    set active [registry::entry active]

    test_equal {[llength $installed]} 5
    test_equal {[llength $active]} 2

    registry::close
    check_throws {registry::entry search}

    registry::open test.db

    set vim [registry::entry active vim]
    test_equal {[$vim version]} 7.1.002

    registry::close

	file delete -force test.db
}

source tests/common.tcl
main $argv
