// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

// Published by .github/workflows/manual.yml at
// https://roboport-tecnologia.github.io/2ksbox/
export default defineConfig({
	site: 'https://roboport-tecnologia.github.io',
	base: '/2ksbox',
	integrations: [
		starlight({
			title: '2ksbox',
			description: 'The 2ksbox user manual: vintage Windows and DOS machines, and Windows 11 beside them.',
			logo: { src: './src/assets/logo.png', alt: '' },
			favicon: '/favicon.png',
			customCss: ['./src/styles/theme.css'],
			social: [
				{ icon: 'github', label: 'Source code', href: 'https://github.com/Roboport-Tecnologia/2ksbox' },
			],
			editLink: {
				baseUrl: 'https://github.com/Roboport-Tecnologia/2ksbox/edit/main/manual/',
			},
			lastUpdated: true,
			sidebar: [
				{
					label: 'Getting started',
					items: ['install', 'first-machine'],
				},
				{
					label: 'Using 2ksbox',
					items: ['discs', '3d', 'controllers', 'snapshots', 'crt-look', 'music', 'windows-11'],
				},
				{
					label: 'Reference',
					items: ['keys', 'troubleshooting', 'privacy', 'acknowledgements'],
				},
			],
		}),
	],
});
