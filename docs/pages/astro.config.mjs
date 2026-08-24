import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://keverza.github.io/tapioca-core',
  base: '/tapioca-core',
  integrations: [
    starlight({
      title: 'Tapioca',
      description: 'Python automation for Archicad 29.',
      logo: {
        src: './src/assets/tapioca-mark.svg',
        alt: 'Tapioca',
      },
      favicon: '/favicon.svg',
      social: [
        {
          icon: 'github',
          label: 'GitHub',
          href: 'https://github.com/keverza/tapioca-core',
        },
      ],
      editLink: {
        baseUrl: 'https://github.com/keverza/tapioca-core/edit/main/docs/pages/',
      },
      customCss: ['./src/styles/custom.css'],
      sidebar: [
        { link: '/', label: 'Overview' },
        { link: 'getting-started', label: 'Getting started' },
        {
          link: 'https://github.com/keverza/tapioca-core/releases',
          label: 'Releases',
        },
        {
          label: 'Authoring',
          items: [
            'commands',
          ],
        },
        {
          label: 'API',
          items: ['api'],
        },
        {
          label: 'Operations',
          items: ['troubleshooting', 'development'],
        },
        {
          label: 'Project',
          items: [
            {
              label: 'Source repository',
              link: 'https://github.com/keverza/tapioca-core',
            },
          ],
        },
      ],
    }),
  ],
});
