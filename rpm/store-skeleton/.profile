# Ghosteel sandbox home — files here persist across launches.
#
# This terminal is the sandboxed Jolla Store build. It sees this home
# folder and the bundled busybox tools, not your SailfishOS system:
# no system apps, no package manager, no root access.
#
# Put your own single-file binaries in ~/bin — it is on PATH and they
# become normal commands. Drop SSH keys into ~/.ssh to use them with a
# bundled or sideloaded ssh client.

export PATH="$HOME/bin:$PATH"
command -v less >/dev/null 2>&1 && export PAGER=less
export EDITOR=vi

# Prompt: honest about where you are
PS1='defaultuser@sandbox $ '
