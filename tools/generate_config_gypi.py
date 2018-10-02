# TODO: assess which if any of the config variables are important to include in
# the js2c'd config.gypi.
import sys

def main(args):
  out = args[0]
  with open(out, 'w') as f:
    f.write("{'variables':{'built_with_electron': 1}}\n")

if __name__ == '__main__':
  main(sys.argv[1:])
