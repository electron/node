import sys

def main(args):
  out = args[0]
  with open(out, 'w') as f:
    f.write("{'variables':{}}\n")

if __name__ == '__main__':
  main(sys.argv[1:])
