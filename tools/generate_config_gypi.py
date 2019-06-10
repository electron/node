# TODO: assess which if any of the config variables are important to include in
# the js2c'd config.gypi.
import sys

def main(out, target_os):
  config = {
    'variables': {
      'built_with_electron': 1,
    }
  }
  if target_os == 'win':
    config['variables']['node_with_ltcg'] = 'true'
  with open(out, 'w') as f:
    f.write(repr(config) + "\n")

if __name__ == '__main__':
  main(*sys.argv[1:])
