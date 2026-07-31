// Exception + iostream smoke test for the Griffin app scaffold.
//
// Go/no-go gate for porting exception-using C++ programs (e.g. the BASIC
// interpreter) to Griffin apps: it exercises unwinding across nested frames
// with destructors, the standard exception types thrown from libstdc++ itself
// (which requires working typeinfo/RTTI), rethrow, and the iostream/fstream
// layer over newlib + the Griffin syscalls.
//
// Every case prints "PASS n ..." on success; the run ends with "ALL PASS".
// Anything missing means unwinding or the C++ runtime is broken in the flat
// app binary.

#include <cerrno>
#include <cstddef>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

#include <unistd.h>

namespace
{

// The file staged on the CF image for case 6.
const char *const kTextFile = "/HELLO.TXT";

// Prints from its destructor so we can see the unwinder actually run cleanup
// code in each abandoned frame.
struct Tracer
{
    const char *name;

    explicit Tracer(const char *n) : name(n)
    {
    }

    ~Tracer()
    {
        std::cout << "    dtor " << name << "\n";
    }
};

struct PlainError
{
    int code;
    const char *message;
};

// ---- case 1: throw a plain struct across several frames with live locals ----

void level3()
{
    Tracer t("level3");
    throw PlainError{42, "thrown from level3"};
}

void level2()
{
    Tracer t("level2");
    level3();
    std::cout << "    UNREACHABLE level2\n";
}

void level1()
{
    Tracer t("level1");
    level2();
    std::cout << "    UNREACHABLE level1\n";
}

bool case1()
{
    try
    {
        Tracer t("case1");
        level1();
    }
    catch (const PlainError &e)
    {
        if (e.code == 42)
        {
            std::cout << "PASS 1 plain struct across frames: " << e.message << "\n";
            return true;
        }
        std::cout << "FAIL 1 wrong code " << e.code << "\n";
        return false;
    }
    std::cout << "FAIL 1 no exception caught\n";
    return false;
}

// ---- case 2: std::runtime_error and what() ----

bool case2()
{
    try
    {
        throw std::runtime_error("runtime_error payload");
    }
    catch (const std::runtime_error &e)
    {
        std::cout << "PASS 2 std::runtime_error: " << e.what() << "\n";
        return true;
    }
    catch (...)
    {
        std::cout << "FAIL 2 caught wrong type\n";
        return false;
    }
}

// ---- case 3: std::stoi failure (thrown from inside libstdc++) ----

bool case3()
{
    try
    {
        int v = std::stoi("xyz");
        std::cout << "FAIL 3 stoi returned " << v << "\n";
        return false;
    }
    catch (const std::invalid_argument &e)
    {
        std::cout << "PASS 3 std::stoi -> std::invalid_argument: " << e.what() << "\n";
        return true;
    }
    catch (const std::exception &e)
    {
        std::cout << "FAIL 3 wrong exception type: " << e.what() << "\n";
        return false;
    }
}

// ---- case 4: std::bad_variant_access (needs typeinfo comparison) ----

bool case4()
{
    std::variant<std::string, double> v{std::string("not a double")};
    try
    {
        double d = std::get<double>(v);
        std::cout << "FAIL 4 std::get returned " << d << "\n";
        return false;
    }
    catch (const std::bad_variant_access &e)
    {
        std::cout << "PASS 4 std::bad_variant_access: " << e.what() << "\n";
        return true;
    }
    catch (const std::exception &e)
    {
        std::cout << "FAIL 4 wrong exception type: " << e.what() << "\n";
        return false;
    }
}

// ---- case 5: nested try + rethrow ----

void inner()
{
    Tracer t("inner");
    try
    {
        Tracer u("inner-try");
        throw std::logic_error("original");
    }
    catch (const std::logic_error &)
    {
        std::cout << "    rethrowing\n";
        throw;
    }
}

bool case5()
{
    try
    {
        inner();
    }
    catch (const std::logic_error &e)
    {
        std::cout << "PASS 5 rethrow through nested try: " << e.what() << "\n";
        return true;
    }
    catch (...)
    {
        std::cout << "FAIL 5 caught wrong type\n";
        return false;
    }
    std::cout << "FAIL 5 no exception caught\n";
    return false;
}

// ---- case 6: iostream / cin / ifstream from CF ----

bool case6()
{
    std::cout << "    cout works\n";

    std::string line;
    if (!std::getline(std::cin, line))
    {
        std::cout << "FAIL 6 getline from cin failed\n";
        return false;
    }
    std::cout << "    echo: [" << line << "]\n";

    std::ifstream in(kTextFile);
    if (!in)
    {
        std::cout << "FAIL 6 cannot open " << kTextFile << "\n";
        return false;
    }
    std::string first;
    if (!std::getline(in, first))
    {
        std::cout << "FAIL 6 cannot read a line from " << kTextFile << "\n";
        return false;
    }
    std::cout << "PASS 6 iostream + cin + ifstream " << kTextFile << ": [" << first << "]\n";
    return true;
}

}  // namespace

// Unbuffered marker, independent of stdio/iostream state.
static void mark(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
    {
        n++;
    }
    write(1, s, n);
}

// libgcc's unwinder calls abort() outright on CFI it cannot handle, and this
// newlib's abort() is a silent _exit(1); intercept it so the gate says so.
extern "C" [[noreturn]] void abort(void)
{
    mark("FAIL abort() called (libgcc unwinder or libc gave up)\r\n");
    _exit(3);
}

// A failed unwind ends in std::terminate -> abort, which on this newlib is a
// silent _exit(1).  Say so instead, so the gate reports a diagnosis.
[[noreturn]] void on_terminate()
{
    std::cout << "FAIL std::terminate called (unwinding did not find the handler)\n";
    std::cout.flush();
    _exit(2);
}

int main()
{
    std::set_terminate(on_terminate);
    std::cout << "exctest: C++ exception and iostream smoke test\n";

    bool ok = true;
    ok = case1() && ok;
    ok = case2() && ok;
    ok = case3() && ok;
    ok = case4() && ok;
    ok = case5() && ok;
    ok = case6() && ok;

    if (ok)
    {
        std::cout << "ALL PASS\n";
    }
    else
    {
        std::cout << "FAILURES\n";
    }
    std::cout.flush();
    return ok ? 0 : 1;
}
