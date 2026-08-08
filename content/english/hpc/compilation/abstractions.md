---
title: Non-Zero-Cost Abstractions
weight: 7
draft: false
---

In general, abstractions are great. Applied well, they reduce the amount of code and the mental burden of a programmer.

But abstractions often come at a cost in terms of performance. A call into a separately compiled shared library may prevent inlining and require dynamic symbol resolution. A virtual call uses an indirect branch whose target may or may not be predictable.

Languages like C++ and Rust heavily promote the idea of *zero-cost abstractions*. This does not mean that every high-level feature costs zero cycles. It means that unused features should cost nothing, and that using an abstraction should be no worse than implementing the same semantics by hand.

The last phrase is important. A growable array cannot have the same cost as a fixed array if growth is actually used, and runtime polymorphism cannot become static dispatch when the concrete type is genuinely unknown. The abstraction may still be optimal for the service it provides.

### When an Abstraction Disappears

Many abstractions really do disappear after inlining and simplification. Consider a small wrapper around an integer:

```c++
struct Number {
    int value;
    int square() const { return value * value; }
};

int square(Number x) {
    return x.square();
}
```

There is no need to allocate an object or perform a function call at run time. `Number` contains only the integer needed by the computation, `square()` is visible to the compiler, and the generated operation is just one multiplication.

Iterators over contiguous memory, small value types, constant-size array wrappers, and ordinary two-argument `std::min` calls are commonly optimized in the same way. Their types help the programmer, while the intermediate representation eventually contains the same loads and arithmetic as hand-written C.

An abstraction stops being zero-cost when it hides information or requires observable behavior that the low-level version does not provide. There are three recurring causes:

- The decision is genuinely made at run time, as with an unknown virtual-call target.
- The implementation is hidden behind a compilation or shared-library boundary.
- The abstraction promises extra semantics such as bounds checks, ownership updates, allocation, or exception propagation.

### Dynamic Dispatch

Any type of runtime polymorphism ultimately has to select code at run time. A virtual call typically loads a function pointer from a table and makes an indirect branch:

```c++
struct Operation {
    virtual int apply(int x) = 0;
};

int transform(Operation *op, int *a, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++)
        sum += op->apply(a[i]);
    return sum;
}
```

The indirect branch is not automatically a misprediction. If `op` keeps the same concrete type, the indirect predictor may learn its target. The larger cost is often that the compiler cannot inline `apply`, and therefore cannot combine its body with the loop or vectorize across calls.

Sometimes the compiler can *devirtualize* the call. This happens when the concrete type is visible after inlining, the class or method is marked `final`, or link-time optimization proves that only one implementation can reach the call site. Then the indirect call becomes a direct call and may disappear entirely.

When devirtualization is impossible, the algorithm can sometimes move dispatch outside the hot loop: select one specialized loop once instead of selecting one operation for every element. This changes the interface and code layout, so the compiler cannot always invent it by itself.

### Bounds Checking

Bounds checking is another real semantic promise, although compilers are good at eliminating it when the proof is local.

```c++
struct Buffer {
    int *data;
    int size;

    int at(int i) const {
        if (i < 0 || i >= size)
            abort();
        return data[i];
    }
};

int sum(Buffer a) {
    int result = 0;
    for (int i = 0; i < a.size; i++)
        result += a.at(i);
    return result;
}
```

After `at` is inlined, the loop condition proves both checks redundant: `i` starts at zero and remains below `a.size`. A compiler can remove them.

The checks may remain if `at` is in another shared library, if the index follows a complicated control path, or if integer overflow makes the proof invalid. The right response is not to delete all checking by reflex. First make the invariant visible through loop structure, types, or [contracts](../contracts), and confirm the result in assembly.

### Generic Code

Generally, complicated code is harder to optimize because each layer has to be inlined and simplified before the important pattern appears.

One example is `std::min`. Its normal two-argument overload is essentially the direct expression `b < a ? b : a` and usually compiles exactly as such. The separate initializer-list overload, used by expressions such as `std::min({a, b, c})`, is implemented by iterating over a temporary range:

```cpp
template<typename _Tp> GLIBCXX14_CONSTEXPR inline _Tp min(initializer_list<_Tp> __l) {
    return *std::min_element(__l.begin(), __l.end());
}
```

A compiler may reduce that too, but it has more work to do, especially when the implementation is not visible.

This distinction matters when judging an abstraction. A slow call named `std::min` does not show that every overload of `std::min` is slow; it shows that a particular interface requested iteration over a run-time range.

Usually it isn't that hard to rewrite a small program so that it is more straightforward and closer to the hardware. If you start removing layers of abstractions, the compiler will eventually give in.

Object-oriented and especially functional languages have some very hard-to-pierce abstractions like these. For this reason, people often prefer to write performance-critical software (interpreters, runtimes, databases) in a style closer to C rather than higher-level languages.

This does not require abandoning types or modularity. It means keeping the hot representation and control flow visible, and placing expensive generality outside the inner loop.

### Memory

Memory abstractions are especially important because the representation determines the access pattern. Consider a matrix represented as a vector of separately allocated rows:

```c++
typedef vector< vector<int> > matrix;
matrix a(n, vector<int>(n, 0));

int val = a[i][j];
```

To read `a[i][j]`, the program first loads the address of row $i$ and then loads element $j$ from that row. The allocations are not guaranteed to be adjacent. This costs an extra dependent load and gives the allocator freedom to scatter rows across cache lines and pages.

A flat allocation needs one address calculation:

```c++
int *a = new int[n * n];
memset(a, 0, sizeof(int) * n * n);

int val = a[i * n + j];
```

Whether the extra indirection is visible in a benchmark depends on cache state and surrounding work, but the two representations are not equivalent. No optimizer may silently turn separately owned, independently resizable rows into one allocation.

You can write a wrapper if you really want an abstraction:

```c++
struct Matrix {
    int *data;
    int stride;

    int *operator[](int row) const {
        return data + row * stride;
    }

    Matrix submatrix(int row, int column) const {
        return {data + row * stride + column, stride};
    }
};
```

The wrapper stores exactly the two values needed for addressing. After inlining, `a[i][j]` becomes the same multiply-and-add used by the flat array.

For example, the [cache-oblivious transposition](/hpc/external-memory/oblivious) for a power-of-two square matrix can use lightweight views without allocating submatrices:

```c++
void transpose(Matrix a, int n) {
    if (n <= 32) {
        for (int i = 0; i < n; i++)
            for (int j = 0; j < i; j++)
                std::swap(a[j][i], a[i][j]);
        return;
    }

    int h = n / 2;
    Matrix A = a.submatrix(0, 0);
    Matrix B = a.submatrix(0, h);
    Matrix C = a.submatrix(h, 0);
    Matrix D = a.submatrix(h, h);

    transpose(A, h);
    transpose(B, h);
    transpose(C, h);
    transpose(D, h);

    for (int i = 0; i < h; i++)
        for (int j = 0; j < h; j++)
            std::swap(B[i][j], C[i][j]);
}
```

The view is a zero-cost abstraction because its semantics already match the desired memory representation. `vector<vector<int>>` is not "failed optimization" here: it deliberately represents a different object, one where rows have separate storage and may have different lengths.

### Library Boundaries

Separate compilation is another information barrier. A compiler that only sees

```c++
int operation(int);
```

must emit a call and obey the platform calling convention. It cannot propagate constants through the function, remove unused work inside it, or specialize it for the caller. A shared-library call may additionally go through a linkage table so that the dynamic loader can select the definition.

Header definitions, static linking, and [link-time optimization](../stages#interprocedural-optimization) expose more implementation to the optimizer. They may improve code, but also increase compilation time and machine-code size. Inlining every abstraction can overflow the instruction cache just as easily as failing to inline one hot function can waste call overhead.

### Finding the Actual Cost

The source-level complexity of an abstraction is not its run-time cost. A ten-line wrapper may disappear, while a one-line copy of an owning object may allocate memory and update reference counts. The reliable procedure is:

1. Identify the hot operation and the semantics it needs.
2. Inspect the resulting assembly or compiler optimization report.
3. Count indirect branches, allocations, dependent loads, checks, and copies that remain.
4. Change the representation or move a run-time decision out of the hot path.
5. Measure again, including code size and cache behavior.

I personally prefer to write low-level code, because it is easier to optimize.

Is it cleaner? Don't think so.

But low-level and unstructured are not synonyms. The best performance abstractions expose the facts the optimizer needs while making the intended data layout and ownership obvious to the human reader too.
