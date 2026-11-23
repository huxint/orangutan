import type { AppState } from './schema.ts';
import { active } from './model.ts';

export type PetId = 'oran' | 'scout' | 'moss' | 'fern' | 'pebble' | 'pip';
export type PetMood = 'idle' | 'thinking' | 'working' | 'awaiting_approval' | 'happy' | 'attention';

export interface CompanionStatus {
  pet: PetId;
  mood: PetMood;
  nameZh: string;
  nameEn: string;
  captionZh: string;
  captionEn: string;
}

const PET_NAMES: Record<PetId, [string, string]> = {
  oran: ['小橙', 'Oran'],
  scout: ['阿探', 'Scout'],
  moss: ['小苔', 'Moss'],
  fern: ['小蕨', 'Fern'],
  pebble: ['石墩', 'Pebble'],
  pip: ['啾啾', 'Pip'],
};

const PET_ACCENTS: Record<PetId, string> = {
  oran: '#b24a1d',
  scout: '#c46a24',
  moss: '#3d6b4a',
  fern: '#476b83',
  pebble: '#8c5e36',
  pip: '#b87d1c',
};

export function petForMemberName(name: string): PetId {
  const lower = name.trim().toLowerCase();
  if (lower.includes('scout') || lower.includes('阿探') || lower.includes('寻叶')) return 'scout';
  if (lower.includes('moss') || lower.includes('苔')) return 'moss';
  if (lower.includes('fern') || lower.includes('蕨')) return 'fern';
  if (lower.includes('pebble') || lower.includes('石墩') || lower.includes('石子')) return 'pebble';
  if (lower.includes('pip') || lower.includes('啾')) return 'pip';
  return 'oran';
}

export function deriveCompanion(
  state: AppState,
  liveCompanion?: PetId,
  now = Date.now(),
): CompanionStatus {
  const localTasks = state.tasks.filter((task) => {
    const conv = state.conversations.find((c) => c.id === task.conversationId);
    return conv?.origin === 'desktop';
  });

  const awaiting = localTasks.find((t) => t.status === 'awaiting_approval');
  if (awaiting) {
    return {
      pet: 'pebble',
      mood: 'awaiting_approval',
      nameZh: PET_NAMES.pebble[0],
      nameEn: PET_NAMES.pebble[1],
      captionZh: '石墩正在复核边界，等你确认这次关键操作',
      captionEn: 'Pebble is waiting for your confirmation',
    };
  }

  const running = localTasks.find((t) => active(t.status));
  if (running) {
    const pet: PetId =
      liveCompanion ??
      (running.kind === 'file' ? 'fern' : running.kind === 'team' ? 'scout' : 'oran');
    return {
      pet,
      mood: running.status === 'running' ? 'working' : 'thinking',
      nameZh: PET_NAMES[pet][0],
      nameEn: PET_NAMES[pet][1],
      captionZh:
        pet === 'fern'
          ? '小蕨正在推进手头的落地子任务'
          : pet === 'scout'
            ? '阿探与小队正在协同调研线索'
            : pet === 'moss'
              ? '小苔正在整理并沉淀长期记忆'
              : '小橙正在拆解目标并协调小队推进',
      captionEn:
        pet === 'fern'
          ? 'Fern is working on the implementation subtask'
          : pet === 'scout'
            ? 'Scout and the squad are investigating'
            : pet === 'moss'
              ? 'Moss is curating long-term notes'
              : 'Oran is splitting the goal and coordinating the squad',
    };
  }

  const dueReminder = (state.reminders ?? []).find((r) => !r.completed && r.dueAt <= now);
  if (dueReminder) {
    return {
      pet: 'pip',
      mood: 'happy',
      nameZh: PET_NAMES.pip[0],
      nameEn: PET_NAMES.pip[1],
      captionZh: `啾啾带来一条提醒：${dueReminder.title}`,
      captionEn: `Pip has a reminder: ${dueReminder.title}`,
    };
  }

  const recentFailed = localTasks.find((t) => t.status === 'failed');
  if (recentFailed && localTasks[0]?.id === recentFailed.id) {
    return {
      pet: 'oran',
      mood: 'attention',
      nameZh: PET_NAMES.oran[0],
      nameEn: PET_NAMES.oran[1],
      captionZh: '小橙遇到一点状况，随时可以重新尝试',
      captionEn: 'Oran hit a snag — ready to try again',
    };
  }

  return {
    pet: 'oran',
    mood: 'idle',
    nameZh: PET_NAMES.oran[0],
    nameEn: PET_NAMES.oran[1],
    captionZh: '小橙端着热茶在桌边，随时可以一起拆分任务',
    captionEn: 'Oran is here with warm tea, ready when you are',
  };
}

export function PetAvatar({
  pet,
  mood = 'idle',
  size = 40,
  className = '',
}: {
  pet: PetId;
  mood?: PetMood;
  size?: number;
  className?: string;
}) {
  const animated = mood === 'working' || mood === 'thinking' || mood === 'happy';
  const classes = `pet-avatar pet-${pet} mood-${mood}${animated ? ' is-animated' : ''}${className ? ` ${className}` : ''}`;
  const accent = PET_ACCENTS[pet];
  const ring =
    mood === 'awaiting_approval'
      ? '0 0 0 2px #b37620, 0 3px 10px rgba(41,33,24,0.16)'
      : mood === 'working' || mood === 'thinking'
        ? `0 0 0 2px ${accent}, 0 3px 10px rgba(41,33,24,0.16)`
        : `0 0 0 1.5px ${accent}44, 0 2px 8px rgba(41,33,24,0.14)`;

  return (
    <img
      src={`/companions/${pet}.jpg`}
      alt={PET_NAMES[pet][1]}
      width={size}
      height={size}
      className={classes}
      style={{
        width: size,
        height: size,
        borderRadius: '50%',
        objectFit: 'cover',
        boxShadow: ring,
        flexShrink: 0,
        display: 'block',
      }}
      loading="lazy"
    />
  );
}
