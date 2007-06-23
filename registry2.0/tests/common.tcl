# Common functions for test cases

proc test {condition} {
    uplevel 1 "\
        if {\[catch {
                if {$condition} { \n\
                } else { \n\
                    puts {Assertion failed: $condition} \n\
                    exit 1 \n\
                } \n\
            } msg\]} { \n\
                puts \"Caught error: \$msg\" \n\
                puts {While executing: $condition}\n\
                exit 1 \n\
            }"
}

proc test_equal {statement value} {
    uplevel 1 "\
        if {\[catch {
                if {$statement == {$value}} { \n\
                } else { \n\
                    puts {Assertion failed: $statement == {$value}} \n\
                    puts \"Actual value: $statement\" \n\
                    exit 1 \n\
                } \n\
            } msg\]} { \n\
                puts \"Caught error: \$msg\" \n\
                puts {While executing: $statement}\n\
                exit 1 \n\
            }"
}

proc check_throws {statement} {
    uplevel 1 "\
        if \{!\[catch $statement\]\} \{ \n\
            puts \{Did not error: $statement\} \n\
            exit 1 \n\
        \}"
}

