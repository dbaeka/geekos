#!/usr/bin/ruby

# takes as input the output of qemu [...] -d exec to find the
# function that encloses the basic block being executed.


$symbol = Hash.new
$cache = Hash.new
File.open("geekos/kernel.syms").each { |ln|
  # 0006085c d APIC_Addr
  if ln =~ /^([0-9a-f]{8}) \w (\S+)/ then
    $symbol[$1.hex] = $2
  end
}

def explain(addr)
  $addresses = $symbol.keys.sort.reverse unless $addresses
  return $cache[addr] if $cache.has_key?(addr) 
  ret = if found = $addresses.bsearch { |x| x <= addr } then
          func = $symbol[found]
          unless [ "APIC_Read", "Get_CPU_ID" ].include?(func)
            "%x %s+%d" % [ addr, func, addr-found ]
          else
            nil
          end
        else
          "%x not found" % [ addr ]
        end
  $cache[addr] = ret
  return ret
end

explain(0x177e7)


File.open(ARGV.length > 0 ? ARGV[0] : "/tmp/qemu.log").each { |ln|
  if ln =~ /^Trace 0x[0-9a-f]+ \[([0-9a-f]{8})\]/ or ln =~ /^EIP=([0-9a-f]{8})/ then
    eip = $1.hex
    if eip > 0xe0000 then
      print "."
    elsif eip < 0x10000 and eip > 0x3000 then
      print ";"
    else
      puts explain(eip)
    end
  end
}

