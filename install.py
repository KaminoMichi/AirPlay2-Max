#!/usr/bin/env python3
from tools.airplayctl import main,Refusal
import sys,subprocess
if __name__=='__main__':
 try:raise SystemExit(main(['install']+sys.argv[1:]))
 except (Refusal,OSError,ValueError,subprocess.TimeoutExpired,KeyError) as e:print('STOP: '+str(e),file=sys.stderr);raise SystemExit(1)
