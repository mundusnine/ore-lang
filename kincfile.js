const release = process.argv.indexOf("--debug") == -1;

const system = platform === Platform.Windows ? "win32" :
			   platform === Platform.Linux   ? "linux" :
			   platform === Platform.OSX     ? "macos" :
			   platform === Platform.HTML5   ? "html5" :
			   platform === Platform.Android ? "android" :
			   platform === Platform.iOS     ? "ios" :
			   								   "unknown";


const libdir = 'Libraries/binaryen/' + system + '/';

let project = new Project('Ore');

project.kore = false;//No Kinc

project.addFile('Sources/**');

project.addIncludeDir("Libraries/binaryen/include");
if (platform === Platform.Windows) {
    project.addLib(libdir);
}
else if (platform === Platform.Linux) {
    project.addLib('binaryen -L../../'+libdir);
}
else if (platform === Platform.OSX) {
    project.addLib(libdir +'libbinaryen.a');
}


resolve(project);
