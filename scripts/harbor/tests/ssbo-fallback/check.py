# SPDX-License-Identifier: GPL-3.0-or-later
import re,sys
text=open(sys.argv[1]).read()
constants={m[1]:int(m[2]) for m in re.finditer(r'(%\d+) = OpConstant %\d+ (\d+)',text)}
functions=re.split(r'%\d+ = OpFunction ',text)[1:]
cases=0
for function in functions:
    for base in (0x1234000000,0x1234000060,0x12340000f0):
        size=0x180
        for addr,expected in ((base-4,False),(base,True),(base+size-4,True),(base+size,False)):
            values=dict(constants); loads=0
            for line in function.splitlines():
                m=re.search(r'(%\d+) = (Op\w+) (.*)',line)
                if not m: continue
                dest,op,tail=m.groups(); args=tail.split()[1:]
                if op=='OpFunctionParameter': values[dest]=addr
                elif op=='OpLoad':
                    values[dest]=base if loads==0 else size
                    loads+=1
                elif op in ('OpBitcast','OpUConvert'): values[dest]=values[args[0]]
                elif op in ('OpBitwiseAnd','OpIAdd','OpISub','OpUGreaterThanEqual','OpULessThan','OpLogicalAnd'):
                    a,b=map(values.__getitem__,args)
                    values[dest]={'OpBitwiseAnd':lambda:a&b,'OpIAdd':lambda:a+b,'OpISub':lambda:a-b,'OpUGreaterThanEqual':lambda:a>=b,'OpULessThan':lambda:a<b,'OpLogicalAnd':lambda:a and b}[op]()
                    if op=='OpLogicalAnd':
                        assert values[dest]==expected,(hex(base),hex(addr),expected,values[dest])
                        cases+=1
                        break
assert cases==72,cases
print(f'PASS: {cases} emitted SPIR-V bounds checks (6 load/store helpers, aligned and unaligned 64-bit addresses)')
