# Put repo-local tools first on PATH.
#
#   source scripts/env.sh
#
# Written for both bash and zsh: the shebang is irrelevant when a file is
# sourced, and BASH_SOURCE does not exist under zsh, where $0 is the script
# path instead.
_lob_root="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
if [ -d "${_lob_root}/.venv/bin" ]; then
  case ":${PATH}:" in
    *":${_lob_root}/.venv/bin:"*) ;;
    *) PATH="${_lob_root}/.venv/bin:${PATH}"; export PATH ;;
  esac
fi
unset _lob_root