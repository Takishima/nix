{
  description = "Hetzner NixOS remote-builder fleet for load-testing the Nix ssh-ng worker protocol";

  # Self-contained flake: it deliberately does NOT depend on the parent Nix
  # package-manager flake at the repo root. comin builds the configurations from
  # this subdirectory (see services.comin.repositorySubdir in builder.nix).
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
    comin = {
      url = "github:nlewo/comin";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    disko = {
      url = "github:nix-community/disko";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      comin,
      disko,
      ...
    }:
    let
      system = "x86_64-linux";

      # The fleet. Each name here becomes networking.hostName, which is how comin
      # picks which nixosConfigurations.<host> a given machine deploys. Add or
      # remove hostnames to scale the fleet, then `git push`.
      builderHosts = [
        "nix-builder-01"
        "nix-builder-02"
      ];

      mkBuilder =
        hostname:
        nixpkgs.lib.nixosSystem {
          inherit system;
          specialArgs = { inherit self; };
          modules = [
            comin.nixosModules.comin
            disko.nixosModules.disko
            ./disko.nix
            ./builder.nix
            { networking.hostName = hostname; }
          ];
        };
    in
    {
      nixosConfigurations = nixpkgs.lib.genAttrs builderHosts mkBuilder;
    };
}
