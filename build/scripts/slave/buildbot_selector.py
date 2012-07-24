#!/usr/bin/python

import os
import subprocess
import sys

THIS_DIR=os.path.dirname(sys.argv[0])


def bash(path):
    return 'bash ' + os.path.join(THIS_DIR, path)

def cmd_call(path):
    return 'call ' + os.path.join(THIS_DIR, path)

BOT_ASSIGNMENT = {
    'win': cmd_call('buildbot_standard.bat'),
    'linux': bash('buildbot_standard.sh'),
    'linux-cmake': bash('buildbot_cmake.sh'),
    'mac10.6': bash('buildbot_standard.sh'),
    'mac10.7': bash('buildbot_standard.sh'),
}

BOT_ADDITIONAL_ENV = {
    'win': {},
    'linux': { 'CHECK_TSAN': '1' },
    'linux-cmake': {},
    'mac10.6': { 'MAX_MAKE_JOBS': '2' },
    'mac10.7': { 'MAX_MAKE_JOBS': '4' },
}

def Main():
  builder = os.environ.get('BUILDBOT_BUILDERNAME')
  cmd = BOT_ASSIGNMENT.get(builder)
  if not cmd:
    sys.stderr.write('ERROR - unset/invalid builder name\n')
    sys.exit(1)

  print "%s runs: %s\n" % (builder, cmd)
  sys.stdout.flush()

  bot_env = os.environ
  add_env = BOT_ADDITIONAL_ENV.get(builder)
  for var in add_env:
    bot_env[var] = add_env[var]

  retcode = subprocess.call(cmd, env=bot_env, shell=True)
  sys.exit(retcode)


if __name__ == '__main__':
  Main()
