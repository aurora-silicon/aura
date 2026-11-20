import struct

from proxyclient.aura.proxy import AuraProxy


def test_cpufreq_wire_opcodes_and_signed_rejection():
    class Interface:
        requests = []
        def proxyreq(self, req, **kwargs):
            opcode,*args=struct.unpack('<7Q',req)
            self.requests.append((opcode,args[:2]))
            return struct.pack('<QqQ',opcode,0,0xffffffffffffffff if opcode==0x1302 else 1260000000)
    iface=Interface()
    p=AuraProxy(iface)
    assert p.cpufreq_get_cluster_hz(1)==1260000000
    assert p.cpufreq_set_cluster_pstate(1,3)==-1
    assert iface.requests==[(0x1301,[1,0]),(0x1302,[1,3])]
