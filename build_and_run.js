const { execSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const VM_NAME = 'BlankOS';
const ISO_PATH = path.join(__dirname, 'blankOS.iso');

// Look for VBoxManage in default Windows installation path or assume it's in PATH
const vboxManagePaths = [
    'VBoxManage', // If it's in PATH
    '"C:\\Program Files\\Oracle\\VirtualBox\\VBoxManage.exe"'
];

let vboxCmd = null;

function runCommand(command, ignoreError = false) {
    try {
        console.log(`\n> ${command}`);
        execSync(command, { stdio: 'inherit' });
        return true;
    } catch (error) {
        if (!ignoreError) {
            console.error(`\n[!] Command failed: ${command}`);
            process.exit(1);
        }
        return false;
    }
}

function findVBoxManage() {
    for (const cmd of vboxManagePaths) {
        try {
            execSync(`${cmd} --version`, { stdio: 'ignore' });
            return cmd;
        } catch (e) {
            // Ignore and try next
        }
    }
    console.error('\n[!] Could not find VirtualBox. Please ensure VirtualBox is installed and VBoxManage is in your system PATH.');
    process.exit(1);
}

console.log('--- Step 1: Building the BlankOS ISO via WSL ---');
// Run WSL to compile the ISO. (Assumes WSL and the make toolchain are installed)
runCommand('wsl make iso');

if (!fs.existsSync(ISO_PATH)) {
    console.error('\n[!] ISO file was not generated successfully. Please check the build errors above.');
    process.exit(1);
}
console.log('\n[+] ISO built successfully: ' + ISO_PATH);

console.log('\n--- Step 2: Configuring VirtualBox ---');
vboxCmd = findVBoxManage();

// Try to unregister and delete the VM if it already exists, so we get a fresh start
console.log('Cleaning up old VM instances if they exist...');
runCommand(`${vboxCmd} unregistervm "${VM_NAME}" --delete`, true);

// Create the VM
console.log('Creating VM...');
runCommand(`${vboxCmd} createvm --name "${VM_NAME}" --ostype "Other_64" --register`);

// Configure VM settings: 2GB RAM, EFI enabled (CRITICAL for BlankOS)
console.log('Applying hardware settings (EFI enabled, 2048MB RAM)...');
runCommand(`${vboxCmd} modifyvm "${VM_NAME}" --memory 2048 --firmware efi --vram 128`);

// Add an IDE controller for the CD/DVD drive
console.log('Adding storage controller...');
runCommand(`${vboxCmd} storagectl "${VM_NAME}" --name "IDE" --add ide`);

// Attach the newly built ISO to the virtual CD/DVD drive
console.log('Attaching BlankOS ISO...');
runCommand(`${vboxCmd} storageattach "${VM_NAME}" --storagectl "IDE" --port 0 --device 0 --type dvddrive --medium "${ISO_PATH}"`);

console.log('\n--- Step 3: Starting VirtualBox ---');
runCommand(`${vboxCmd} startvm "${VM_NAME}"`);

console.log('\n[+] Success! BlankOS is now booting in VirtualBox.');
