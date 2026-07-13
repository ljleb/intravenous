import { LanePresentationPlugin } from "./protocol";
import { discoveredLanePresentationPlugins } from "./generated";

export const lanePresentationPlugins: readonly LanePresentationPlugin[] = discoveredLanePresentationPlugins;
