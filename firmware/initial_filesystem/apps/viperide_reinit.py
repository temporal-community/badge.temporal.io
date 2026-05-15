if True:
    import sys
    import os
    print(sys.ps1)
    try: u=os.uname()
    except: u=('','','','',sys.platform)
    try: v=sys.version.split(';')[1].strip()
    except: v='MicroPython '+u[2]
    mpy=getattr(sys.implementation, '_mpy', 0)
    sp=':'.join(sys.path)
    d=[u[4],u[2],u[0],v,mpy>>10,mpy&0xFF,(mpy>>8)&3,sp]
    print('|'.join(str(x) for x in d))

    s = os.statvfs('/')
    fs = s[1] * s[2]
    ff = s[3] * s[0]
    fu = fs - ff
    print('%s|%s|%s'%(fu,ff,fs))

    def walk(p):
        for n in os.listdir(p if p else '/'):
            fn=p+'/'+n
            try: s=os.stat(fn)
            except: s=(0,)*7
            try:
                if s[0] & 0x4000 == 0:
                    print('f|'+fn+'|'+str(s[6]))
                elif n not in ('.','..'):
                    print('d|'+fn+'|'+str(s[6]))
                    walk(fn)
            except:
                print('f|'+p+'/???|'+str(s[6]))
    walk('')