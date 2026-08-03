{
  description = "neoswc - neuswc fork implementing river-window-management-v1";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    neu-nix = {
      # neuwld is not in nixpkgs; this flake is where it and neuswc come from.
      # Pinned to the revision the host system uses so the fork builds against
      # exactly the toolchain upstream does.
      url = "github:ricardomaps/neu-nix/8078f344b0e95c5719f692396d7b4fe01a0d51bd";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    { self, nixpkgs, neu-nix }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        overlays = [ neu-nix.overlays.default ];
      };

      # Our fork, built by upstream's own derivation with the source swapped.
      # example=true builds example/wm, the test compositor the VM runs.
      neoswc = (pkgs.neuswc.override { example = true; }).overrideAttrs (old: {
        pname = "neoswc";
        src = self;

        # example/meson.build builds wm without install:true, so it never
        # reaches $out/bin. Install it here under a non-generic name rather
        # than patching upstream's file -- "wm" is far too broad for $PATH.
        postInstall = (old.postInstall or "") + ''
          wm=$(find "$NIX_BUILD_TOP" -type f -name wm -perm -u+x | head -1)
          [ -n "$wm" ] || { echo "example/wm not found; is -Dexample=true set?" >&2; exit 1; }
          install -Dm755 "$wm" $out/bin/neoswc-example-wm
        '';

        doCheck = true;
        checkPhase = ''
          runHook preCheck
          meson test --print-errorlogs
          runHook postCheck
        '';
      });
    in
    {
      packages.${system} = {
        inherit neoswc;
        default = neoswc;

        # Headless QEMU guest that runs neoswc on virtio-gpu. swc has no nested
        # backend, so this is how the compositor gets exercised without taking
        # over a VT on the host. See CLAUDE.md.
        vm = self.nixosConfigurations.testvm.config.system.build.vm;
      };

      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ neoswc ];
      };

      nixosConfigurations.testvm = nixpkgs.lib.nixosSystem {
        inherit system;
        modules = [
          # Provides virtualisation.* and system.build.vm. Only pulled in by
          # nixos-rebuild build-vm otherwise.
          "${nixpkgs}/nixos/modules/virtualisation/qemu-vm.nix"
          ./test/vm/configuration.nix
          { _module.args.neoswc = neoswc; }
        ];
      };
    };
}
